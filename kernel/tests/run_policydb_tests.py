import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


def run(command, log):
    with log.open('wb') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'{log}: exit {result.returncode}')


def linux(path):
    path = path.resolve()
    return '/mnt/' + path.drive[0].lower() + path.as_posix()[2:] if os.name == 'nt' else str(path)


def wsl(command):
    return ['wsl', '-d', 'Ubuntu', '--', *command] if os.name == 'nt' else command


def cpio(files):
    data = bytearray()
    entries = [('dev', stat.S_IFDIR | 0o755, b'', 0, 0),
               ('dev/console', stat.S_IFCHR | 0o600, b'', 5, 1)]
    entries += [(name, stat.S_IFREG | 0o755, path.read_bytes(), 0, 0) for name, path in files]
    entries += [('TRAILER!!!', 0, b'', 0, 0)]
    for inode, (name, mode, contents, major, minor) in enumerate(entries, 1):
        name = name.encode() + b'\0'
        fields = [inode, mode, 0, 0, 1, 0, len(contents), 0, 0, major, minor, len(name), 0]
        data += b'070701' + ''.join(f'{value:08x}' for value in fields).encode() + name
        data += b'\0' * (-len(data) % 4)
        data += contents
        data += b'\0' * (-len(data) % 4)
    return data + b'\0' * (-len(data) % 512)


parser = argparse.ArgumentParser()
parser.add_argument('--out', required=True)
parser.add_argument('--kmi', default='android16-6.12')
parser.add_argument('--secilc', default='secilc')
parser.add_argument('--standalone', type=Path)
parser.add_argument('--build-only', action='store_true')
parser.add_argument('--fixed-parser', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
tests = Path(__file__).resolve().parent
out = Path(args.out).resolve()
out.mkdir(parents=True, exist_ok=True)
for name in ('policydb_probe.c', 'policydb_alloc.h'):
    shutil.copyfile(tests / name, out / name)
policy_root = args.standalone.resolve() if args.standalone else root
standalone = (policy_root / 'kernel/host/policy.c').is_file()
policy_dir = policy_root / ('kernel/host' if standalone else 'kernel/selinux')
shutil.copyfile(policy_dir / 'policydb_cleanup.h', out / 'policydb_cleanup.h')
source = (policy_dir / ('policy.c' if standalone else 'sepolicy.c')).read_text()
def definition(name, unit=source):
    match = re.search(r'^(?:static\s+)?(?:struct selinux_policy\s*\*\s*|(?:int|void|bool)\s+)' +
                      name + r'\([^;{}]+\)\s*\{', unit, re.M)
    if not match:
        raise ValueError(f'cannot locate production definition of {name}')
    function = unit[match.start():unit.index('\n}', match.end()) + 2]
    return function if function.startswith('static ') else 'static ' + function


name = 'yz_policy_dup' if standalone else 'ksu_dup_sepolicy'
snapshot = 'ksu_write_sepolicy_snapshot(' in source
functions = ['ksu_write_sepolicy_snapshot', 'ksu_read_sepolicy_snapshot'] if snapshot else []
clone = '\n\n'.join(definition(f) for f in [*functions, name])
clone = clone.replace(name, 'test_dup').replace('yz_call_policydb_', 'policydb_')
wrapped = ['kmalloc', 'kzalloc', 'kmalloc_array', 'kcalloc', 'kvcalloc', 'vmalloc',
           'kmem_cache_zalloc', 'kmem_cache_free', 'kfree', 'kvfree', 'vfree', 'kstrdup',
           'kmemdup', 'policydb_read', 'policydb_write']


def instrument(code):
    return re.sub(r'\b(' + '|'.join(wrapped) + r')\s*\(', lambda m: 'test_' + m[1] + '(', code)


initial_snapshot = not standalone and (policy_root == root or snapshot)
if initial_snapshot:
    rules = (policy_dir / 'rules.c').read_text()
    metadata = source[source.index('static DEFINE_SEQLOCK(ksu_original_types_lock)'):
                      source.index('static void invalidate_original_types_locked(')]
    startup = [definition(f) for f in ('invalidate_original_types_locked', 'ksu_set_original_types',
                                       'forget_original_types', 'ksu_destroy_sepolicy')]
    startup.extend(definition(f, rules) for f in ('backup_original_sepolicy', 'dup_policy_for_kernel_rules',
                                                  'ksu_detach_backup_sepolicy'))
    (out / 'initial_snapshot_impl.c').write_text(metadata + instrument('\n\n'.join(startup)) + '\n', newline='\n')
    clone = '#define TEST_INITIAL_SNAPSHOT 1\n' + clone
(out / 'clone_impl.c').write_text('#include <linux/overflow.h>\n#include <linux/version.h>\n' +
                                  instrument(clone) + '\n', newline='\n')
run(wsl([args.secilc, '-M', 'true', '-c', '33', '-o', linux(out / 'fixture.bin'),
         '-f', '/dev/null', linux(tests / 'policydb_fixture.cil')]), out / 'fixture.log')
fixture = (out / 'fixture.bin').read_bytes()
(out / 'fixture.h').write_text('static const unsigned char fixture[] = {' +
                             ','.join(str(b) for b in fixture) + '};\n', newline='\n')
large_cil = (tests / 'policydb_fixture.cil').read_text() + ''.join(
    f'(type extra_{i})\n' for i in range(4096))
(out / 'large.cil').write_text(large_cil, newline='\n')
run(wsl([args.secilc, '-M', 'true', '-c', '33', '-o', linux(out / 'large.bin'),
         '-f', '/dev/null', linux(out / 'large.cil')]), out / 'large.log')
with (out / 'fixture.h').open('a') as stream:
    stream.write('static const unsigned char large_fixture[] = {' +
                 ','.join(str(b) for b in (out / 'large.bin').read_bytes()) + '};\n')
if args.fixed_parser:
    (out / 'fixed-parser').touch()
elif (out / 'fixed-parser').exists():
    (out / 'fixed-parser').unlink()
shutil.copyfile(tests / 'prepare_policydb_modpost.sh', out / 'prepare_modpost.sh')
(out / 'prepare.py').write_text(r'''import hashlib,json,os,re,subprocess
from pathlib import Path
build=Path('/probe'); kdir=Path(os.environ['KDIR']); src=Path('/opt/ddk/src')/kdir.name
names=['policydb','hashtab','symtab','ebitmap','avtab','conditional','mls','context','sidtab']
symbols={'security_get_initial_sid_context'}
for n in names:
    text=subprocess.check_output(['llvm-nm','--defined-only','--extern-only',str(kdir/'security/selinux/ss'/f'{n}.o')],text=True)
    for line in text.splitlines():
        words=line.split()
        if len(words)==3 and not words[-1].startswith('__'):
            symbols.add(words[-1])
(build/'namespace.h').write_text(''.join(f'#define {s} probe_{s}\n' for s in sorted(symbols)))
if (build/'fixed-parser').exists():
    with (build/'namespace.h').open('a') as f: f.write('#define TEST_FIXED_PARSER 1\n')
allocators=['kmalloc','kzalloc','kmalloc_array','kcalloc','kvcalloc','vmalloc','kmem_cache_zalloc','kmem_cache_free','kfree','kvfree','vfree','kstrdup','kmemdup']
hashes={}
for n in names:
    p=src/'security/selinux/ss'/f'{n}.c'; text=p.read_text();hashes[n]=hashlib.sha256(p.read_bytes()).hexdigest()
    if n == 'policydb' and (build/'fixed-parser').exists():
        a=text.index('int policydb_read('); b=text.index('\nbad:',a)
        body=text[a:b]; assert body.count('goto out;') == 2
        text=text[:a]+body.replace('goto out;','goto bad;')+text[b:]
    text=re.sub(r'\b('+'|'.join(allocators)+r')\s*\(',lambda m:'test_'+m[1]+'(',text)
    text=re.sub(r'"(avtab_node|avtab_extended_perms|ebitmap_node|hashtab_node)"',r'"probe_\1"',text)
    lines=text.splitlines(); last=max(i for i,l in enumerate(lines) if l.startswith('#include '))
    lines.insert(last+1,'#include "policydb_alloc.h"\n#undef pr_err\n#define pr_err(...) do {} while (0)')
    (build/f'{n}.c').write_text('\n'.join(lines)+'\n')
(build/'parser-sources.json').write_text(json.dumps(hashes,indent=2))
(build/'Makefile').write_text('''+repr('''obj-m := policydb_test.o
policydb_test-y := policydb_probe.o $(or $(PARSER_OBJECTS),probe_parser.o)
ccflags-y += -I$(srctree)/security/selinux -I$(srctree)/security/selinux/ss -I$(srctree)/security/selinux/include -I$(objtree)/security/selinux -I$(src)
ccflags-y += -include $(src)/namespace.h -ffunction-sections -fdata-sections
''')+r''')
''', newline='\n')
(out / 'build.sh').write_text('''set -eu
cd /probe
python3 /probe/prepare.py
bash /probe/prepare_modpost.sh
# Older Kbuild includes its symbol and CRC flags in the MODPOST variable.
install -m 755 /probe/modpost-tool/modpost "$KDIR/scripts/mod/modpost"
objects="policydb.o hashtab.o symtab.o ebitmap.o avtab.o conditional.o mls.o context.o sidtab.o"
make -C "$KDIR" M=/probe ARCH=arm64 LLVM=1 CC=clang PARSER_OBJECTS="$objects" -j8 $objects policydb_probe.o
roots=$(llvm-nm -u policydb_probe.o | awk '$NF ~ /^probe_/ {print "-u", $NF}')
ld.lld -r --gc-sections $roots $objects -o probe_parser.o
printf 'source_/probe/probe_parser.o := /probe/policydb.c\n' > /probe/.probe_parser.o.cmd
make -C "$KDIR" M=/probe ARCH=arm64 LLVM=1 CC=clang modules
cp "$KDIR/arch/arm64/boot/Image" /probe/Image
''', newline='\n')
run(['docker', 'run', '--rm', '--pull=never', '--entrypoint', 'bash',
     '--mount', f'type=bind,source={out},target=/probe',
     f'ghcr.io/ylarod/ddk:{args.kmi}-20260828', '/probe/build.sh'], out / 'build.log')
if args.build_only:
    raise SystemExit(0)
(out / 'init.c').write_text('''#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
int main(void) {
  int fd = open("/probe.ko", O_RDONLY);
  if (fd < 0 || syscall(SYS_finit_module, fd, "", 0)) {
    perror("POLICYDB_GUEST_FAIL");
  } else puts("POLICYDB_GUEST_PASS");
  fflush(stdout);
  reboot(RB_POWER_OFF);
  return 1;
}
''', newline='\n')
run(wsl(['aarch64-linux-gnu-gcc', '-static', '-O2', linux(out / 'init.c'), '-o', linux(out / 'init')]), out / 'init.log')
(out / 'init.cpio').write_bytes(cpio([('init', out / 'init'), ('probe.ko', out / 'policydb_test.ko')]))
run(wsl(['timeout', '180', 'qemu-system-aarch64', '-machine', 'virt,gic-version=3,mte=on',
         '-cpu', 'max', '-accel', 'tcg', '-smp', '2', '-m', '1536', '-nodefaults',
         '-nographic', '-serial', 'stdio', '-monitor', 'none', '-nic', 'none', '-no-reboot',
         '-kernel', linux(out / 'Image'), '-initrd', linux(out / 'init.cpio'), '-append',
         'console=ttyAMA0 rdinit=/init panic=-1 panic_on_warn=1 kasan=on kaslr.disabled=1']), out / 'guest.log')
text = (out / 'guest.log').read_text(errors='replace')
passes = ['POLICYDB_TEST_PASS', 'POLICYDB_GUEST_PASS']
if initial_snapshot:
    passes.append('POLICYDB_INITIAL_SNAPSHOT_PASS')
if not all(s in text for s in passes) or any(
        s in text for s in ('POLICYDB_TEST_FAIL', 'BUG:', 'WARNING:', 'Kernel panic')):
    raise RuntimeError(f'{out / "guest.log"}: guest failed')
print('PASS: kernel parser allocation sweep, truncated inputs and production clone')

import argparse
import hashlib
import json
import os
from pathlib import Path
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
name = 'yz_policy_dup' if standalone else 'ksu_dup_sepolicy'
start = source.index(('static ' if standalone else '') + 'struct selinux_policy *' + name + '(')
end = source.index('\n}', start) + 2
clone = source[start:end].replace(name, 'test_dup').replace('yz_call_policydb_', 'policydb_')
allocators = ['kmalloc', 'kzalloc', 'kmalloc_array', 'kcalloc', 'kvcalloc', 'vmalloc',
              'kmem_cache_zalloc', 'kmem_cache_free', 'kfree', 'kvfree', 'vfree', 'kstrdup', 'kmemdup']
import re
clone = re.sub(r'\b(' + '|'.join(allocators) + r')\s*\(', lambda m: 'test_' + m[1] + '(', clone)
(out / 'clone_impl.c').write_text('#include <linux/overflow.h>\n#include <linux/version.h>\n' + clone + '\n', newline='\n')
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
names=['policydb','hashtab','symtab','ebitmap','avtab','conditional','mls','context']
symbols=set()
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
policydb_test-y := policydb_probe.o parser.o
ccflags-y += -I$(srctree)/security/selinux -I$(srctree)/security/selinux/ss -I$(srctree)/security/selinux/include -I$(objtree)/security/selinux -I$(src)
ccflags-y += -include $(src)/namespace.h -ffunction-sections -fdata-sections
''')+r''')
''', newline='\n')
(out / 'build.sh').write_text('''set -eu
cd /probe
python3 /probe/prepare.py
bash /probe/prepare_modpost.sh
make -C "$KDIR" M=/probe ARCH=arm64 LLVM=1 CC=clang -j8 policydb.o hashtab.o symtab.o ebitmap.o avtab.o conditional.o mls.o context.o
ld.lld -r --gc-sections -u probe_policydb_read -u probe_policydb_destroy -u probe_policydb_write -u probe_ebitmap_cache_init -u probe_hashtab_cache_init -u probe_avtab_cache_init policydb.o hashtab.o symtab.o ebitmap.o avtab.o conditional.o mls.o context.o -o parser.o_shipped
printf 'source_/probe/parser.o := /probe/policydb.c\n' > /probe/.parser.o.cmd
make -C "$KDIR" M=/probe ARCH=arm64 LLVM=1 CC=clang MODPOST=/probe/modpost-tool/modpost modules
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
if 'POLICYDB_TEST_PASS' not in text or 'POLICYDB_GUEST_PASS' not in text or any(
        s in text for s in ('POLICYDB_TEST_FAIL', 'BUG:', 'WARNING:', 'Kernel panic')):
    raise RuntimeError(f'{out / "guest.log"}: guest failed')
print('PASS: kernel parser allocation sweep, truncated inputs and production clone')

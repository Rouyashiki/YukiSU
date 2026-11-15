import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--out', required=True)
parser.add_argument('--standalone', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
target = args.standalone.resolve() if args.standalone else root
standalone = (target / 'kernel/host/policy.c').is_file()
source = target / ('kernel/feature/runtime.c' if standalone else 'kernel/feature/yukizygisk/runtime.c')
text = source.read_text()
name = 'yz_runtime_report' if standalone else 'ksu_yukizygisk_report_runtime'
start = text.index('int ' + name + '(')
end = text.index('\n}', start) + 2
out = Path(args.out).resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'report_impl.c').write_text(text[start:end].replace(name + '(', 'test_report(', 1), newline='\n')
def linux(p):
    return '/mnt/' + p.drive[0].lower() + p.as_posix()[2:] if os.name == 'nt' else str(p)
prefix = ['wsl', '-d', 'Ubuntu', '--'] if os.name == 'nt' else []
includes = target / 'kernel' if standalone else (target / 'kernel/include' if (target / 'kernel/include/uapi/yukizygisk.h').exists() else target)
subprocess.run(prefix + ['gcc', '-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined',
    '-I' + linux(includes), '-I' + linux(out), linux(root / 'kernel/tests/runtime_outcomes_test.c'),
    '-o', linux(out / 'test')], check=True)
subprocess.run(prefix + [linux(out / 'test')], check=True)

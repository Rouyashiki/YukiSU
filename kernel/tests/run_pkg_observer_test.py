import argparse
import os
from pathlib import Path
import subprocess


def extract_function(source, signature):
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    return source[start:end]


def linux_path(path):
    if os.name == 'nt':
        return '/mnt/' + path.drive[0].lower() + path.as_posix()[2:]
    return str(path)


parser = argparse.ArgumentParser()
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--observer-source', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
observer_path = args.observer_source or root / 'kernel/manager/pkg_observer.c'
observer = observer_path.read_text(encoding='utf-8')
tracker = (root / 'kernel/manager/throne_tracker.c').read_text(encoding='utf-8')
functions = [
    extract_function(tracker, 'void track_throne(bool prune_only)'),
    extract_function(tracker, 'void ksu_request_manager_rescan(void)'),
    extract_function(observer, 'static void ksu_track_throne_tw_func('),
    extract_function(observer, 'static void ksu_defer_track_throne(void)'),
    extract_function(observer, 'static int ksu_handle_inode_event('),
]
out = args.out.resolve()
out.mkdir(parents=True, exist_ok=True)
(out / 'pkg_observer_impl.c').write_text(
    '\n\n'.join(functions) + '\n', encoding='utf-8', newline='\n'
)
prefix = ['wsl', '-d', 'Ubuntu', '--'] if os.name == 'nt' else []
binary = out / 'pkg_observer_test'
subprocess.run(prefix + [
    'gcc', '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-parameter', '-fsanitize=address,undefined',
    '-I' + linux_path(out),
    linux_path(root / 'kernel/tests/pkg_observer_test.c'),
    '-o', linux_path(binary),
], check=True)
subprocess.run(prefix + [linux_path(binary)], check=True)

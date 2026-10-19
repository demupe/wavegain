"""Build and run the make check gate using a native MinGW/LLVM-MinGW compiler.

Example: python tools/check_windows.py --cc aarch64-w64-mingw32-clang --optimized
Run from a developer shell where the compiler's runtime DLLs are on PATH.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--optimized', action='store_true', help='Use Clang -O3 and ThinLTO')
    parser.add_argument('--ubsan', action='store_true', help='Check undefined behavior and float-to-int overflow')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Use make check on POSIX systems')
    compiler = str(Path(args.cc).resolve()) if Path(args.cc).exists() else args.cc
    # POSIX shlex is used by the existing Python test drivers.
    cc_arg = shlex.quote(compiler.replace('\\', '/'))
    flags = ['-std=gnu17', '-O3' if args.optimized else '-O2',
             '-D_FILE_OFFSET_BITS=64', '-DHAVE_CONFIG_H', '-D_FORTIFY_SOURCE=2']
    if args.optimized:
        flags.append('-flto=thin')
    if args.ubsan:
        flags += ['-g', '-fsanitize=undefined,float-cast-overflow', '-fno-sanitize-recover=all']
    libs = ['-lm']
    target = subprocess.check_output([compiler, '-dumpmachine'], text=True).strip()
    if 'mingw' in target:
        libs += ['-Wl,-Bstatic', '-lssp', '-Wl,-Bdynamic']

    def run(command):
        print('Running: ' + subprocess.list2cmdline(list(map(str, command))), flush=True)
        subprocess.run(command, cwd=ROOT, check=True)

    def python(script, *options):
        run([sys.executable, script, *options])

    binary = ROOT / 'wavegain.exe'
    run([compiler, *flags, '-o', binary, *sorted((ROOT / 'src').glob('*.c')), *libs])
    python('test/run_suite.py', '--cc', cc_arg, '--cflags=' + shlex.join(flags),
           '--ldflags=' + shlex.join(libs), '--binary', str(binary))
    print('Native Windows check gate passed.', flush=True)


if __name__ == '__main__':
    main()

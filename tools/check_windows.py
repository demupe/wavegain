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
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--optimized', action='store_true', help='Use Clang -O3 and ThinLTO')
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
    run([compiler, *flags, '-o', binary, *sorted(ROOT.glob('*.c')), *libs])
    python('tools/generate_help.py', '--check')
    python('test/filter_regressions.py', '--cc', cc_arg, '--cflags=' + shlex.join(flags),
           '--ldflags=' + shlex.join(libs), '--cli', str(binary))
    python('test/loudness_kernels.py', '--cc', cc_arg, '--cflags=' + shlex.join(flags),
           '--ldflags=' + shlex.join(libs))
    tests = {
        'portability': ['test/portability.c', 'w64.c'],
        'wave64': ['test/wave64.c'],
        'review_io': ['test/review_io.c', 'w64.c', 'wavegain.c', 'gain_analysis.c',
                      'dither.c', 'misc.c', 'loudness.c', 'resample.c', 'output.c'],
        'feature_dsp': ['test/feature_dsp.c', 'resample.c'],
        'gain_kernel': ['test/gain_kernel.c', 'audio.c', 'w64.c', 'gain_analysis.c',
                        'dither.c', 'misc.c', 'loudness.c', 'resample.c', 'output.c'],
        'windows_errors': ['test/windows_errors.c', 'misc.c', 'output.c'],
        'output_publish': ['test/output_publish.c'],
    }
    # Windows emulation/scanners can briefly retain an exited executable.
    # A cleanup lock must not prevent the remaining regression tests running.
    with tempfile.TemporaryDirectory(prefix='wavegain-windows-check-', ignore_cleanup_errors=True) as temp:
        for name, sources in tests.items():
            executable = Path(temp) / (name + '.exe')
            run([compiler, *flags, '-UNDEBUG', '-o', executable, *sources, *libs])
            run([executable])
    for script in ('todo_regressions', 'review_regressions', 'feature_regressions'):
        python(f'test/{script}.py')
    print('Native Windows check gate passed.', flush=True)


if __name__ == '__main__':
    main()

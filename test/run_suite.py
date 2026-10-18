"""Single test inventory for make check and the native Windows build wrapper."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
from test_support import ROOT, DEFAULT_BINARY, resolve_binary

# Each test's included .c files are intentionally excluded from its link list.
C_TESTS = {
    'portability': ['test/portability.c', 'w64.c'],
    'wave64': ['test/wave64.c'],
    'review_io': ['test/review_io.c', 'w64.c', 'wavegain.c', 'gain_analysis.c',
                  'dither.c', 'misc.c', 'loudness.c', 'resample.c', 'output.c'],
    'feature_dsp': ['test/feature_dsp.c', 'resample.c'],
    'gain_kernel': ['test/gain_kernel.c', 'audio.c', 'w64.c', 'gain_analysis.c',
                    'dither.c', 'misc.c', 'loudness.c', 'resample.c', 'output.c'],
    'windows_errors': ['test/windows_errors.c', 'misc.c', 'output.c'],
    'output_publish': ['test/output_publish.c'],
    'command_regressions': ['test/command_regressions.c',
                            *[p.name for p in sorted(ROOT.glob('*.c')) if p.name != 'main.c']],
}
CLI_TESTS = ('todo_regressions', 'review_regressions', 'feature_regressions',
             'followup_regressions', 'edge_regressions', 'windows_paths', 'windows_files')
CORE = ('docs', 'support_contracts', 'filters', 'loudness', *C_TESTS, *CLI_TESTS)
INTEROP = ('wave64_interop', 'filter_interop', 'riff_interop', 'feature_interop')
WINDOWS_ONLY = {'windows_errors', 'command_regressions', 'windows_paths', 'windows_files'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cflags', default='-O2 -D_FILE_OFFSET_BITS=64 -DHAVE_CONFIG_H')
    parser.add_argument('--ldflags', default='-lm')
    parser.add_argument('--binary', type=Path, default=DEFAULT_BINARY)
    parser.add_argument('--suite', choices=('core', 'interop'), default='core')
    parser.add_argument('--only', action='append', choices=(*CORE, *INTEROP),
                        help='Run a named test; may be repeated')
    parser.add_argument('--list', action='store_true', help='List tests without compiling or executing')
    args = parser.parse_args()
    inventory = CORE if args.suite == 'core' else INTEROP
    if args.only and any(name not in inventory for name in args.only):
        parser.error('--only test must belong to the selected --suite')
    selected = [name for name in inventory if not args.only or name in args.only]
    if args.list:
        for name in selected:
            print(name + (' [Windows only]' if name in WINDOWS_ONLY else ''))
        return
    if args.suite == 'interop':
        missing = [tool for tool in ('ffmpeg', 'ffprobe') if not shutil.which(tool)]
        if missing:
            parser.error('Interop suite requires ' + ', '.join(missing) + '; no interop tests executed')

    def run(command):
        print('Running: ' + subprocess.list2cmdline(list(map(str, command))), flush=True)
        env = dict(os.environ)
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)

    def python(script, *options):
        run([sys.executable, script, *options])

    # Temporary executable paths avoid stale in-tree test binaries and make -j races.
    with tempfile.TemporaryDirectory(prefix='wavegain-check-', ignore_cleanup_errors=True) as temp:
        for name in selected:
            if name in WINDOWS_ONLY and os.name != 'nt':
                print(f'SKIP {name}: Windows-only test', flush=True)
                continue
            if name == 'docs':
                python('tools/generate_help.py', '--check')
            elif name == 'support_contracts':
                python('test/support_contracts.py')
            elif name in ('filters', 'loudness'):
                script = 'filter_regressions' if name == 'filters' else 'loudness_kernels'
                options = ['--cc', args.cc, '--cflags=' + args.cflags, '--ldflags=' + args.ldflags]
                if name == 'filters':
                    options += ['--cli', str(resolve_binary(args.binary))]
                python(f'test/{script}.py', *options)
            elif name in C_TESTS:
                binary = Path(temp) / (name + ('.exe' if os.name == 'nt' else ''))
                run([*shlex.split(args.cc), *shlex.split(args.cflags), '-UNDEBUG',
                     '-o', binary, *C_TESTS[name], *shlex.split(args.ldflags)])
                run([binary])
            else:
                python(f'test/{name}.py', '--binary', str(resolve_binary(args.binary)))
            print('PASS ' + name, flush=True)
    print(f'{args.suite} suite passed.', flush=True)


if __name__ == '__main__':
    main()

"""Run the shared core inventory from a VS 2022+ Developer Command Prompt.

Use the x64 or x86 developer prompt to select the native compiler target.
The C source/link inventory is shared with make check, not duplicated here.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'test'))
from run_suite import C_TESTS, CLI_TESTS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cl')
    parser.add_argument('--binary', type=Path, help='Test this existing CLI instead of building one')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('MSVC checks require Windows')

    def run(command, **kwargs):
        print('Running: ' + subprocess.list2cmdline(list(map(str, command))), flush=True)
        return subprocess.run(list(map(str, command)), cwd=ROOT, check=True, **kwargs)

    with tempfile.TemporaryDirectory(prefix='wavegain-msvc-') as temp:
        folder = Path(temp)

        def build(name, sources, defines=()):
            target = folder / (name + '.exe')
            run([args.cc, '/nologo', '/std:c17', '/O2', '/fp:precise', '/UNDEBUG',
                 '/DHAVE_CONFIG_H', '/D_CRT_SECURE_NO_WARNINGS',
                 *['/D' + value for value in defines], '/Fo' + str(folder) + os.sep,
                 '/Fe' + str(target), *sources])
            return target

        binary = args.binary.resolve() if args.binary else build('wavegain', sorted((ROOT / 'src').glob('*.c')))
        for script in ('tools/generate_help.py', 'test/support_contracts.py'):
            run([sys.executable, script, *(['--check'] if script.startswith('tools/') else [])])
        for script, variants in [
            ('filter_regressions', [('scalar', ['WAVEGAIN_MONO_REUSE=0']), ('default', [])]),
            ('loudness_kernels', [('reference', ['WG_EBUR_REFERENCE']),
                                  ('previous', ['WG_EBUR_EXTRA_REFERENCE']), ('default', [])])]:
            hashes = []
            for name, defines in variants:
                target = build(script + '-' + name, ['test/' + script + '.c'], defines)
                result = run([target], stdout=subprocess.PIPE)
                hashes.append(hashlib.sha256(result.stdout).hexdigest())
            if len(set(hashes)) != 1:
                raise RuntimeError(script + ': native reference and optimized state differ')
            print('PASS ' + script, flush=True)
        for name, sources in C_TESTS.items():
            inputs = [source if source.startswith('test/') else ROOT / 'src' / source for source in sources]
            run([build(name, inputs)])
            print('PASS ' + name, flush=True)
        for name in CLI_TESTS:
            run([sys.executable, 'test/' + name + '.py', '--binary', binary])
            print('PASS ' + name, flush=True)
    print('Native MSVC core suite passed.', flush=True)


if __name__ == '__main__':
    main()

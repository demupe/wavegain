"""Native Windows directory-link cycles and physical output containment."""
import argparse
import base64
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import wave
from test_support import add_binary_argument, check_result, resolve_binary

ROOT = Path(__file__).resolve().parents[1]


def fixture(path):
    with wave.open(str(path), 'wb') as out:
        out.setparams((1, 2, 44100, 0, 'NONE', 'not compressed'))
        out.writeframes(struct.pack('<44100h', *([1000, -2000, 3000, -1000] * 11025)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    add_binary_argument(parser)
    parser.add_argument('--case', choices=('all', 'recursion', 'containment'), default='all')
    args = parser.parse_args()
    if os.name != 'nt':
        print('Windows directory-link tests: skipped on POSIX')
        return
    args.binary = resolve_binary(args.binary)
    with tempfile.TemporaryDirectory(prefix='wavegain-windows-paths-') as temp:
        base = Path(temp).resolve()
        links = []

        def junction(link, target):
            # Both ends are fixtures in this test's named temporary directory.
            assert link.parent.resolve().is_relative_to(base)
            assert target.resolve().is_relative_to(base)
            # Pass paths as environment data, never interpolate them into shell
            # code (user/temp directory names can contain &, %, quotes, etc.).
            script = ('New-Item -ItemType Junction -Path $env:WG_TEST_JUNCTION_LINK '
                      '-Target $env:WG_TEST_JUNCTION_TARGET -ErrorAction Stop | Out-Null')
            encoded = base64.b64encode(script.encode('utf-16-le')).decode('ascii')
            env = dict(os.environ, WG_TEST_JUNCTION_LINK=str(link), WG_TEST_JUNCTION_TARGET=str(target))
            r = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-EncodedCommand', encoded],
                               env=env, capture_output=True)
            assert r.returncode == 0, r.stdout + r.stderr
            links.append(link)

        def run(*options):
            result = subprocess.run([str(args.binary), *map(str, options)], capture_output=True, timeout=15)
            return check_result(result, success=result.returncode == 0)

        try:
            if args.case in ('all', 'recursion'):
                inputs = base / 'inputs'; inputs.mkdir()
                child = inputs / 'child'; child.mkdir()
                outside = base / 'outside'; outside.mkdir()
                fixtures = [inputs / 'root.wav', child / 'deep.wav', outside / 'excluded.wav']
                for file in fixtures:
                    fixture(file)
                hashes = [hashlib.sha256(f.read_bytes()).hexdigest() for f in fixtures]
                junction(inputs / 'loop', inputs)
                junction(inputs / 'external', outside)
                r = run('-c', '-z', inputs)
                assert r.returncode == 0, r.stderr
                rows = re.findall(rb'^\s*[+-][0-9.]+ dB\s*\|', r.stderr, re.MULTILINE)
                assert len(rows) == 2 and b'excluded.wav' not in r.stderr, r.stderr
                assert [hashlib.sha256(f.read_bytes()).hexdigest() for f in fixtures] == hashes
                # An explicitly selected directory link remains a valid input.
                junction(base / 'explicit-alias', inputs)
                r = run('-c', '-z', base / 'explicit-alias')
                assert r.returncode == 0 and len(re.findall(rb'^\s*[+-][0-9.]+ dB\s*\|', r.stderr, re.MULTILINE)) == 2, r.stderr
                print('Windows recursion: cycles/external junctions skipped; real children and explicit linked roots work.', flush=True)
            if args.case in ('all', 'containment'):
                inputs = base / 'contained-input'; inputs.mkdir()
                source = inputs / 'source.wav'; fixture(source)
                original = source.read_bytes()
                alias = base / 'input & %alias%'; junction(alias, inputs)
                # Output through another path spelling must still be rejected.
                for input_path, output in ((inputs, alias / 'derived'), (alias, inputs / 'derived-reverse'),
                                           (inputs, alias)):
                    r = run('-y', '-z', '--output-dir', output, input_path)
                    assert r.returncode != 0 and b'outside input directories' in r.stderr, r.stderr
                    assert not list(output.glob('*.wav')) or output.resolve() == inputs.resolve()
                    assert source.read_bytes() == original
                # A linked output directory genuinely outside input is allowed.
                outside = base / 'allowed-output'; outside.mkdir()
                output_alias = base / 'allowed (output) & 100%'; junction(output_alias, outside)
                r = run('-y', '-z', '--output-dir', output_alias, inputs)
                assert r.returncode == 0 and (outside / 'source.wav').exists(), r.stderr
                assert source.read_bytes() == original
                expected = base / 'expected.wav'; expected.write_bytes(original)
                r = run('-y', expected)
                assert r.returncode == 0 and (outside / 'source.wav').read_bytes() == expected.read_bytes(), r.stderr
                print('Windows containment: physical aliases rejected inside input; external linked output has identical audio bytes.', flush=True)
        finally:
            # Unlink junction entries only; never recursively delete their targets.
            for link in reversed(links):
                assert link.parent.resolve().is_relative_to(base)
                os.rmdir(link)


if __name__ == '__main__':
    main()

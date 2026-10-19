"""Regression cases for all six findings from the 2026-10-18 audit."""
import math
import os
from pathlib import Path
import struct
import tempfile
from audio_fixtures import fmt, wav, w64, audio
from test_support import parse_binary, run_cli

ROOT = Path(__file__).resolve().parents[1]
BIN = parse_binary(__doc__)


def run(*args, success=True, stdin=None):
    return run_cli(BIN, args, success=success, input_data=stdin)


def rejected(path, data, *args):
    path.write_bytes(data)
    run(*args, path, success=False)
    assert path.read_bytes() == data, path


def main():
    pcm = b''.join(struct.pack('<hh', round(5000 * math.sin(i * .14)), round(4000 * math.cos(i * .17)))
                   for i in range(22050))
    options = ['-y', '-p', '-n', '--force', '-g', '-6']
    with tempfile.TemporaryDirectory(prefix='wavegain-followup-', ignore_cleanup_errors=True) as temp:
        folder = Path(temp)
        path = folder / 'input.wav'
        # Non-invertible scales must be rejected in both containers and chunk positions.
        for scale in (0., -1., float('nan'), float('inf'), 5e-324):
            rejected(path, wav(pcm, gain=scale), '--undo-gain')
            for tail in (False, True):
                rejected(path, w64(pcm, scale, tail), '--undo-gain')
        for scale in (.5, 2.):
            for data in (wav(pcm, gain=scale), w64(pcm, scale)):
                path.write_bytes(data)
                run('--undo-gain', path)
                assert path.read_bytes() != data
        rejected(path, w64(pcm, 1e101), '--undo-gain')  # Preserve the legacy Wave64 upper bound.
        # Every partial-frame remainder, including float and Wave64 inputs.
        for bits, floating in ((8, False), (16, False), (24, False), (32, False), (32, True)):
            block = bits // 8 * 2
            complete = bytes(block * 22050)
            for remainder in range(1, block):
                data = complete + bytes(remainder)
                rejected(path, wav(data, fmt(bits, floating=floating)), *options)
                rejected(path, w64(data, format_bytes=fmt(bits, floating=floating)), *options)
        path.write_bytes(wav(bytes(22051), fmt(8, channels=1)))
        run(*options, path)  # Valid odd-length mono 8-bit data and RIFF padding.
        # Valid-bit metadata and declared extension size.
        for valid, cb in ((17, 22), (16, 21), (16, 23)):
            rejected(path, wav(pcm, fmt(valid=valid, cb=cb)), *options)
            rejected(path, w64(pcm, format_bytes=fmt(valid=valid, cb=cb)), *options)
        rejected(path, wav(pcm, fmt(valid=16)[:-1]), *options)
        floating = struct.pack('<ff', .1, -.1) * 22050
        rejected(path, wav(floating, fmt(32, floating=True, valid=24)), *options)
        for bits, valid in ((16, 0), (24, 20), (32, 24)):
            packed = b''.join((int(v) << (bits - 16)).to_bytes(bits // 8, 'little', signed=True)
                              for v in struct.unpack('<' + 'h' * (len(pcm) // 2), pcm))
            outputs = []
            for extension in (None, valid):
                path.write_bytes(wav(packed, fmt(bits, valid=extension)))
                run(*options, path)
                outputs.append(audio(path.read_bytes()))
            assert outputs[0] == outputs[1], (bits, valid)
        path.write_bytes(wav(pcm, fmt(valid=16, cb=24, extra=b'xx')))
        run('-c', path)  # Additional extension bytes within the actual fmt chunk.
        # Duplicate paths and hard-link identities fail before rewriting a batch.
        original = wav(pcm)
        path.write_bytes(original)
        for alias in (path, str(path.parent) + '/./' + path.name):
            run(*options, path, alias, success=False)
            assert path.read_bytes() == original
        alias = folder / 'alias.wav'
        os.link(path, alias)
        run(*options, path, alias, success=False)
        assert path.read_bytes() == alias.read_bytes() == original
        alias.unlink()
        # Across recursive batches, the earlier file is processed once, never twice.
        directory = folder / 'album'
        directory.mkdir()
        song = directory / 'song.wav'
        song.write_bytes(original)
        path.write_bytes(original)
        run(*options, path)
        once = path.read_bytes()
        run(*options, '-z', song, directory, success=False)
        assert song.read_bytes() == once
        # Distinct files sharing a basename remain valid.
        other_directory = folder / 'other'
        other_directory.mkdir()
        other = other_directory / 'song.wav'
        song.write_bytes(original); other.write_bytes(original)
        run(*options, song, other)
        assert song.read_bytes() == other.read_bytes() == once
        # Float DC beyond int32 is still safe in album and log formatting.
        path.write_bytes(wav(struct.pack('<f', 100000.) * 44100, fmt(32, channels=1, floating=True)))
        log = folder / 'dc.log'
        result = run('-c', '-q', '-f', log, path)
        assert b'-3276800000' in result.stderr and b'-2147483648' not in result.stderr
        assert '-3276800000' in log.read_text()
        if os.name == 'nt':
            result = run('-c', '-q', path, '-e', 'echo', '%DC_OFFSET_L%')
            assert b'-3276800000' in result.stdout, result
            before = path.read_bytes()
            run('-y', path, '-e', 'echo', 'x' * 9000, success=False)
            assert path.read_bytes() == before
            run('-c', path, '-e', 'echo', 'wow!', success=False)
            # File and folder names expand as data, never as cmd.exe syntax.
            hostile = folder / 'd&echo INJECTED&'
            hostile.mkdir()
            for name in (folder / 'a&echo INJECTED&.wav', folder / 'b^(x)%PATH%!y.wav', hostile / 'c.wav'):
                name.write_bytes(original)
                for command in (['echo %INPUT_FILE%'], ['echo', '%INPUT_FDIR%', '%INPUT_NAME%']):
                    result = run('-c', '-q', name, '-e', *command)
                    lines = result.stdout.decode(errors='replace').splitlines()
                    assert not any(line.strip() == 'INJECTED' for line in lines), result
                    assert name.stem in result.stdout.decode(errors='replace'), result
                assert name.read_bytes() == original
    print('Follow-up regressions passed: gain metadata, frame bounds, extensible PCM, duplicate identities, DC and command limits')


if __name__ == '__main__':
    main()

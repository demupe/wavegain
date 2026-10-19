"""CLI stream boundaries, legacy malformed fixtures and cross-batch alias safety."""
import math
import os
from pathlib import Path
import struct
import tempfile
from audio_fixtures import fmt, wav, w64, audio, chunk
from test_support import ROOT, parse_binary, run_cli

BIN = parse_binary(__doc__)


def main():
    pcm = b''.join(struct.pack('<hh', round(5000 * math.sin(i * .13)), round(4000 * math.cos(i * .19)))
                   for i in range(4411))
    original = wav(pcm)
    options = ['-y', '-p', '-n', '--force', '-g', '-6']
    with tempfile.TemporaryDirectory(prefix='wavegain-edges-', ignore_cleanup_errors=True) as temp:
        folder = Path(temp)
        path = folder / 'input.wav'
        # Repository crash/memory probes must fail normally, including on apply.
        for name in ('floating_point_exception.wav', 'memory_leak.wav'):
            raw = (ROOT / 'test' / name).read_bytes()
            path.write_bytes(raw)
            run_cli(BIN, ['-c', path], success=False)
            run_cli(BIN, [*options, path], success=False)
            assert path.read_bytes() == raw
        # Malformed finite stdin size must fail; neither RIFF padding nor EOF
        # recovery for seekable files makes incomplete frames valid.
        for data in (wav(pcm + b'x'), w64(pcm + b'x'), original[:-4]):
            run_cli(BIN, ['-c', '-'], success=False, input_data=data)
        path.write_bytes(original[:-4])  # Complete frame at physical EOF: documented recovery.
        run_cli(BIN, ['-c', path])
        rf_body = (b'WAVE' + chunk(b'ds64', struct.pack('<QQQI', len(original) + 28,
                                                      len(pcm), len(pcm) // 4, 0))
                   + chunk(b'fmt ', fmt()) + b'data' + struct.pack('<I', 0xffffffff) + pcm)
        rf64 = b'RF64' + struct.pack('<I', 0xffffffff) + rf_body
        # Make Wave64 data a multiple of eight bytes so a cut removes PCM,
        # rather than the optional padding of its final chunk.
        for source in (original, rf64, w64(pcm + pcm[:4])):
            for cut in (1, 4):
                truncated = source[:-cut]
                path.write_bytes(truncated)
                result = run_cli(BIN, ['-c', path], success=cut == 4)
                if cut == 1:
                    assert b'incomplete PCM frame' in result.stderr
                assert path.read_bytes() == truncated
        # stdout must contain only the finalized container, never status text.
        for bit_format in ('1', '2', '3', '4', '5', '6'):
            path.write_bytes(original)
            run_cli(BIN, [*options, '-b', bit_format, path])
            expected = path.read_bytes()
            path.write_bytes(original)
            result = run_cli(BIN, [*options, '-b', bit_format, '-o', path])
            assert result.stdout == expected, ('stdout container mismatch', bit_format)
            assert path.read_bytes() == original
        path.write_bytes(w64(pcm))
        run_cli(BIN, [*options, path])
        expected = path.read_bytes()
        path.write_bytes(w64(pcm))
        result = run_cli(BIN, [*options, '-o', path])
        assert result.stdout == expected and path.read_bytes() == w64(pcm)
        # -o plus --rf64 intentionally emits ordinary finite RIFF for small audio.
        path.write_bytes(original)
        result = run_cli(BIN, [*options, '--rf64', '-o', path])
        assert result.stdout[:4] == b'RIFF' and len(audio(result.stdout)) == len(pcm)
        assert path.read_bytes() == original
        # A hard link retains the original ID after the first name is replaced.
        # The invocation-wide guard must still stop that alias in the next batch.
        album = folder / 'album'; album.mkdir()
        song = album / 'song.wav'
        song.write_bytes(original)
        first = folder / 'first.wav'
        os.link(song, first)
        path.write_bytes(original)
        run_cli(BIN, [*options, path])
        once = path.read_bytes()
        run_cli(BIN, [*options, '-z', first, album], success=False)
        assert first.read_bytes() == once and song.read_bytes() == original
        # Valid extended IEEE float (including unspecified precision) remains usable.
        floating = struct.pack('<ff', .1, -.2) * 4411
        for valid in (0, 32):
            path.write_bytes(wav(floating, fmt(32, floating=True, valid=valid)))
            run_cli(BIN, ['-c', path])
        # LUFS minimum duration is inclusive, for both file and stdin analysis.
        for frames, success in ((17639, False), (17640, True)):
            data = wav((pcm * 4)[:frames * 4])
            path.write_bytes(data)
            run_cli(BIN, ['-c', '-p', '--lufs', path], success=success)
            run_cli(BIN, ['-c', '-p', '--lufs', '-'], input_data=data, success=success)
            assert path.read_bytes() == data
        # Numeric option bounds and quantization-room rejection are separate contracts.
        for option, values in (('--target-lufs', ('-70', '-5')), ('--peak-ceiling', ('-60', '0'))):
            for value in values:
                run_cli(BIN, ['-c', '-p', option, value, path])
        for option, values in (('--target-lufs', ('-70.001', '-4.999', 'nan', 'inf', 'abc')),
                               ('--peak-ceiling', ('-60.001', '.001', 'nan', 'inf', 'abc'))):
            for value in values:
                run_cli(BIN, ['-y', option, value, path], success=False)
                assert path.read_bytes() == data
        run_cli(BIN, ['-c', '-p', '--peak-ceiling', '-60', '-b', '1', path], success=False)
        assert path.read_bytes() == data
    print('Edge regressions passed: legacy crash probes, strict streams, exact stdout, retained alias IDs, LUFS duration and numeric bounds')


if __name__ == '__main__':
    main()

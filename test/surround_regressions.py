"""Surround PCM preservation, gain policy, LFE protection and mixed album DC."""
import math
from pathlib import Path
import re
import struct
import tempfile
from audio_fixtures import GUID, wav, w64
from test_support import parse_binary, run_cli

BINARY = parse_binary(__doc__)


def fixture(path, channels=8, mask=0x63f, bits=16, floating=False, rate=48000,
            frames=24017, amplitudes=None, dc=None, scale=None, rf64=False):
    amplitudes = amplitudes or [.04 + .004 * ch for ch in range(channels)]
    dc = dc or [0.] * channels
    values = [dc[ch] + amplitudes[ch] * math.sin(2 * math.pi * (1000 if ch != 3 else 60) * i / rate)
              for i in range(frames) for ch in range(channels)]
    if floating:
        pcm = struct.pack('<' + 'f' * len(values), *values)
    else:
        integers = [max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1,
                    round(v * (1 << (bits - 1))))) for v in values]
        pcm = b''.join(bytes([v + 128]) if bits == 8 else
                       v.to_bytes(bits // 8, 'little', signed=True) for v in integers)
    block = channels * (bits // 8)
    fmt = struct.pack('<HHIIHHHHI', 0xfffe, channels, rate, rate * block,
                      block, bits, 22, bits, mask)
    fmt += bytes([3 if floating else 1]) + GUID[1:]
    data = w64(pcm, scale=scale, format_bytes=fmt) if path.suffix == '.w64' else wav(pcm, fmt, gain=scale)
    if rf64:
        # Finalized small RF64 fixture, with a real ds64 frame count.
        size = len(data) + 36
        ds64 = b'ds64' + struct.pack('<IQQQI', 28, size - 8, len(pcm), frames, 0)
        data = b'RF64' + b'\xff' * 4 + data[8:12] + ds64 + data[12:]
        pos = data.index(b'data', 48)
        data = data[:pos + 4] + b'\xff' * 4 + data[pos + 8:]
    path.write_bytes(data)
    return values


def chunks(data):
    wide = data[:4] == b'riff'
    pos = 40 if wide else 12
    while pos < len(data):
        size = struct.unpack_from('<Q' if wide else '<I', data, pos + (16 if wide else 4))[0]
        offset = 24 if wide else 8
        yield data[pos:pos + 4], data[pos + offset:pos + size if wide else pos + offset + size]
        pos += ((size + 7) // 8 * 8) if wide else offset + size + size % 2


def read(path):
    parts = dict(chunks(path.read_bytes()))
    fmt, pcm = parts[b'fmt '], parts[b'data']
    tag, channels, rate, _, block, bits = struct.unpack_from('<HHIIHH', fmt)
    assert tag == 0xfffe and len(fmt) >= 40
    assert struct.unpack_from('<HH', fmt, 16) == (22, bits)
    assert block == channels * bits // 8
    floating = fmt[24] == 3
    assert fmt[24:] == bytes([3 if floating else 1]) + GUID[1:]
    width = bits // 8
    values = list(struct.unpack('<' + 'f' * (len(pcm) // 4), pcm)) if floating else [
        ((pcm[i] - 128) if width == 1 else int.from_bytes(pcm[i:i + width], 'little', signed=True)) /
        (1 << (bits - 1)) for i in range(0, len(pcm), width)]
    return channels, rate, bits, struct.unpack_from('<I', fmt, 20)[0], values


def loudness(path):
    result = run_cli(BINARY, ['--lufs', '-p', path], text=True)
    return float(re.search(r'Integrated loudness:\s*(\S+) LUFS', result.stderr)[1])


with tempfile.TemporaryDirectory(prefix='wavegain-surround-') as tmp:
    folder = Path(tmp)
    source = folder / 'source.wav'
    fixture(source)
    original = source.read_bytes()
    for options in ([], ['-y'], ['--true-peak'], ['--fast']):
        result = run_cli(BINARY, [*options, source], success=False, text=True)
        assert 'requires --lufs' in result.stderr
        assert source.read_bytes() == original
    for options in (['--limiter'], ['-b', '6']):
        run_cli(BINARY, ['-y', '--lufs', *options, source], success=False)
        assert source.read_bytes() == original
    # Refusing one surround file still allows a supported stereo input to run.
    stereo = folder / 'stereo.wav'
    stereo.write_bytes(wav(struct.pack('<hh', 3000, -3000) * 24017))
    mixed_out = folder / 'mixed'
    run_cli(BINARY, ['-y', '--output-dir', mixed_out, source, stereo], success=False)
    assert not (mixed_out / source.name).exists()
    assert (mixed_out / stereo.name).is_file()
    # Layout errors are refused without touching inputs or publishing outputs.
    for channels, mask in ((8, 0), (8, 0xff), (6, 0x63f), (7, 0x13f), (9, 0x63f)):
        invalid = folder / 'invalid.wav'
        fixture(invalid, channels=channels, mask=mask)
        before = invalid.read_bytes()
        run_cli(BINARY, ['-y', '--lufs', invalid], success=False)
        assert invalid.read_bytes() == before
    # A plain multichannel PCM fmt has no reliable speaker layout.
    block = 8 * 2
    invalid.write_bytes(wav(bytes(24017 * block), struct.pack('<HHIIHH',
                            1, 8, 48000, 48000 * block, block, 16)))
    run_cli(BINARY, ['--lufs', invalid], success=False)

    # LFE energy never changes integrated loudness, but it still caps gain.
    fixture(source, amplitudes=[.003] * 8)
    quiet_lfe = loudness(source)
    fixture(source, amplitudes=[.003] * 3 + [.98] + [.003] * 4)
    assert loudness(source) == quiet_lfe
    for mode in ('fast', 'precise', 'extended'):
        target = folder / f'peak-{mode}.wav'
        result = run_cli(BINARY, ['-y', '-p', '--target-lufs', '-23', '-n',
                                 '--true-peak-mode', mode, '--output', target, source], text=True)
        actual_peak = float(re.search(r'Output true peak:\s*(\S+) dBTP', result.stderr)[1])
        assert actual_peak <= -1.
        src, dst = read(source)[4], read(target)[4]
        factor = dst[8 * 200 + 3] / src[8 * 200 + 3]
        assert factor < 1.
        assert max(abs(a - b * factor) for a, b in zip(dst, src)) < 4 / 32768
    fixture(source, amplitudes=[0.] * 3 + [.1] + [0.] * 4)
    result = run_cli(BINARY, ['--lufs', '-p', source], text=True)
    assert 'manual gain only' in result.stderr

    # All supported layouts, source containers and output widths retain their map.
    for channels, mask in ((6, 0x3f), (6, 0x60f), (8, 0x63f)):
        for container in ('wav', 'rf64', 'w64'):
            source = folder / ('source.w64' if container == 'w64' else 'source.wav')
            fixture(source, channels=channels, mask=mask, rf64=container == 'rf64')
            for format_id, bits in enumerate((8, 16, 24, 32, 32), 1):
                target = folder / f'out-{channels}-{mask}-{container}-{format_id}.{source.suffix[1:]}'
                run_cli(BINARY, ['-y', '-p', '--target-lufs', '-30', '-b', format_id,
                                 '--output', target, source])
                ch, rate, width, actual_mask, values = read(target)
                assert (ch, rate, width, actual_mask) == (channels, 48000, bits, mask)
                assert len(values) == 24017 * channels
                assert abs(loudness(target) + 30) < (.5 if bits == 8 else .03)
                # Every output, including converted float, is valid input again.
                if format_id == 5:
                    second = folder / f'float-roundtrip-{channels}-{mask}-{container}{source.suffix}'
                    run_cli(BINARY, ['-y', '-p', '--lufs', '--output', second, target])
                    assert read(second)[:4] == (channels, 48000, 32, mask)

    # Decode each integer source width, and finalize exact stdout headers.
    for bits in (8, 16, 24, 32):
        source = folder / f'width-{bits}.wav'
        fixture(source, bits=bits)
        target = folder / f'width-out-{bits}.wav'
        result = run_cli(BINARY, ['-y', '-p', '--lufs', '--stdout', source])
        target.write_bytes(result.stdout)
        assert read(target)[:4] == (8, 48000, bits, 0x63f)
        assert len(read(target)[4]) == 24017 * 8
    source = folder / 'stdout.w64'
    fixture(source)
    target = folder / 'stdout-out.w64'
    target.write_bytes(run_cli(BINARY, ['-y', '-p', '--lufs', '--stdout', source]).stdout)
    assert read(target)[:4] == (8, 48000, 16, 0x63f)
    assert len(read(target)[4]) == 24017 * 8

    # Non-finite PCM in LFE or the final side channel must also be refused.
    for channel in (3, 7):
        source = folder / 'nonfinite.wav'
        fixture(source, bits=32, floating=True)
        data = bytearray(source.read_bytes())
        start = data.index(b'data') + 8
        struct.pack_into('<f', data, start + channel * 4, float('nan'))
        source.write_bytes(data)
        run_cli(BINARY, ['-y', '--lufs', source], success=False)
        assert source.read_bytes() == data

    # Undo does not require LUFS; every channel returns to its original level.
    source = folder / 'undo-source.wav'
    fixture(source)
    gained, undone = folder / 'gained.wav', folder / 'undone.wav'
    run_cli(BINARY, ['-y', '-p', '-w', '--lufs', '--output', gained, source])
    run_cli(BINARY, ['--undo-gain', '--output', undone, gained])
    assert read(undone)[:4] == read(source)[:4]
    assert max(abs(a - b) for a, b in zip(read(undone)[4], read(source)[4])) < 4 / 32768

    # Partial dither blocks cover all eight histories and both profiles.
    fixture(source, scale=1.)
    for profile in ('legacy', 'rate'):
        for dither in (1, 2, 3, 4):
            target = folder / f'dither-{profile}-{dither}.wav'
            options = ['--dither-profile', profile] if profile == 'rate' else []
            run_cli(BINARY, ['--undo-gain', '--force', '-d', dither, *options,
                             '--output', target, source])
            assert max(abs(a - b) for a, b in zip(read(target)[4], read(source)[4])) < 64 / 32768

    # Mixed layouts: a side channel's DC must not mix with a packed back index.
    back, side, full = [folder / name for name in ('back.wav', 'side.wav', 'full.wav')]
    fixture(back, channels=6, mask=0x3f, amplitudes=[0.] * 6, dc=[.01, .02, .03, .04, .10, .12])
    fixture(side, channels=6, mask=0x60f, amplitudes=[0.] * 6, dc=[.01, .02, .03, .04, -.10, -.12])
    fixture(full, amplitudes=[0.] * 8, dc=[.01, .02, .03, .04, .10, .12, -.10, -.12])
    outdir = folder / 'album'
    run_cli(BINARY, ['-y', '-a', '-q', '--lufs', '--output-dir', outdir, back, side, full])
    for path in (back, side, full):
        assert max(map(abs, read(outdir / path.name)[4])) < 2 / 32768

    # High sample rates stay native; stdin uses the first-pass layout mapping.
    fixture(source, rate=192000, frames=76801, floating=True, bits=32)
    assert math.isfinite(loudness(source))
    result = run_cli(BINARY, ['--lufs', '-p', '-'], input_data=source.read_bytes())
    assert b'Integrated loudness:' in result.stderr
    assert not list(folder.glob('*.tmp_*'))

print('Surround CLI regressions passed')

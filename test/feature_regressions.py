"""CLI contracts for separate output, LUFS/true peak, high rates and rate dither."""
import math
import os
from pathlib import Path
import re
import struct
import stat
import tempfile
import wave
from test_support import parse_binary, run_cli
from audio_fixtures import audio

ROOT = Path(__file__).resolve().parents[1]
BINARY = parse_binary(__doc__)
GAIN = re.compile(r'Applying Gain of\s*([+-]?[\d.]+) dB')
LUFS = re.compile(r'Integrated loudness:\s*([+-]?[\d.]+) LUFS')
INPUT_PEAK = re.compile(r'Input true peak after DC correction:\s*([+-]?[\d.]+)')


def run(*flags, ok=True, input_data=None):
    r = run_cli(BINARY, flags, success=ok, input_data=input_data)
    text = r.stderr.decode(errors='replace')
    return r, text


def fixture(path, rate=48000, seconds=1., amplitude=.1, dc=0., square=False):
    count = round(rate * seconds)
    values = [round(32767 * (dc + amplitude * ((1 if i % 4 < 2 else -1) if square else
                                            math.sin(2 * math.pi * 1000 * i / rate)))) for i in range(count)]
    with wave.open(str(path), 'wb') as f:
        f.setparams((1, 2, rate, 0, 'NONE', 'PCM'))
        f.writeframes(struct.pack(f'<{count}h', *values))


def reconstructed_peak(values):
    """Independent windowed-sinc reconstruction of a stationary interior."""
    peak, radius, center = 0., 256, len(values) // 2
    for step in range(5 * 64):
        t = center + step / 64
        base, value = int(t), 0.
        for k in range(base - radius + 1, base + radius + 1):
            d = t - k
            sinc = math.sin(math.pi * d) / (math.pi * d) if d else 1.
            window = .42 + .5 * math.cos(math.pi * d / radius) + .08 * math.cos(2 * math.pi * d / radius)
            value += values[k] * sinc * window
        peak = max(peak, abs(value))
    return peak


with tempfile.TemporaryDirectory(prefix='wavegain-features-') as temp:
    folder = Path(temp)
    source = folder / 'input.wav'
    fixture(source)
    original = source.read_bytes()
    # Preserve ordinary input permissions even under a restrictive output umask.
    if os.name != 'nt':
        for mask, mode in ((0o022, 0o644), (0o077, 0o640), (0o022, 0o600), (0o077, 0o444)):
            source.chmod(mode)
            saved_mask = os.umask(mask)
            try:
                destination = folder / f'permission-{mask}-{mode}.wav'
                run('-y', '--output', destination, source)
            finally:
                os.umask(saved_mask)
            assert stat.S_IMODE(destination.stat().st_mode) == mode
            assert source.read_bytes() == original
        source.chmod(0o644)
    # Sub-LSB offsets must display as integer zero on stderr and in the log.
    for dc in (0., .00001, -.00001):
        fixture(source, dc=dc)
        log = folder / 'dc.log'
        _, display = run('-f', log, source)
        for text in (display, log.read_text()):
            rows = [line.split('|') for line in text.splitlines()
                    if line.count('|') == 6 and line.rstrip().endswith(str(source))]
            assert rows and all(row[4].strip() == row[5].strip() == '0' for row in rows), text
    source.write_bytes(original)
    _, direct = run('--lufs', '--true-peak', '-p', source)
    _, piped = run('--lufs', '--true-peak', '-p', '-', input_data=original)
    assert LUFS.search(direct).group(1) == LUFS.search(piped).group(1)
    run('--lufs', '-', input_data=original, ok=False)
    target = folder / 'created' / 'output.wav'
    run('-y', '--output', target, source)
    assert source.read_bytes() == original and target.exists()
    inplace = folder / 'inplace.wav'; inplace.write_bytes(original)
    run('-y', inplace)
    assert target.read_bytes() == inplace.read_bytes()
    before = target.read_bytes()
    run('-y', '--output', target, source, ok=False)
    assert target.read_bytes() == before and source.read_bytes() == original
    run('-y', '--output', source, source, ok=False)
    # Recursive output preserves relative subdirectories and never revisits output.
    inputs = folder / 'inputs'; (inputs / 'a' / 'b').mkdir(parents=True)
    fixture(inputs / 'root.wav'); fixture(inputs / 'a' / 'b' / 'deep.wav')
    if os.name != 'nt':
        (inputs / 'root.wav').chmod(0o640)
        (inputs / 'a' / 'b' / 'deep.wav').chmod(0o600)
    outputs = folder / 'outputs'
    run('-y', '-z', '--output-dir', outputs, inputs)
    assert (outputs / 'root.wav').exists() and (outputs / 'a' / 'b' / 'deep.wav').exists()
    if os.name != 'nt':
        assert stat.S_IMODE((outputs / 'root.wav').stat().st_mode) == 0o640
        assert stat.S_IMODE((outputs / 'a' / 'b' / 'deep.wav').stat().st_mode) == 0o600
    run('-y', '-z', '--output-dir', inputs / 'nested-output', inputs, ok=False)
    # Multiple explicit files with a common basename cannot overwrite one another.
    second_dir = folder / 'second'; second_dir.mkdir()
    second = second_dir / 'input.wav'; second.write_bytes(original)
    collision = folder / 'collision'
    run('-y', '--output-dir', collision, source, second, ok=False)
    assert not (collision / 'input.wav').exists()
    # Existing and dangling symlinks are never replaced.
    if hasattr(Path, 'symlink_to'):
        link = folder / 'link.wav'
        link.symlink_to(source)
        run('-y', '--output', link, source, ok=False)
        assert link.is_symlink() and source.read_bytes() == original
        link.unlink(); link.symlink_to(folder / 'nonexistent.wav')
        run('-y', '--output', link, source, ok=False)
        assert link.is_symlink()
    # LUFS target is applied as one fixed gain, with explicit peak limiting.
    result, text = run('-y', '--lufs', '--true-peak', '--output', folder / 'lufs.wav', source)
    measured = float(LUFS.search(text).group(1))
    applied = float(GAIN.search(text).group(1))
    assert abs(measured + applied - -23.) < .02, text
    assert 'Output true peak:' in text
    # Explicit precise is the default, while fast changes only peak measurement.
    source_bytes = source.read_bytes()
    for quality in ('precise', 'fast', 'extended'):
        target = folder / f'lufs-{quality}.wav'
        _, selected = run('-y', '--lufs', '--true-peak-mode', quality, '--output', target, source)
        assert f'True-peak mode: {quality}' in selected
        assert LUFS.search(selected).group(1) == LUFS.search(text).group(1)
        assert target.read_bytes() == (folder / 'lufs.wav').read_bytes()
        _, direct = run('--lufs', '--true-peak-mode', quality, '-p', source)
        _, piped = run('--lufs', '--true-peak-mode', quality, '-p', '-', input_data=source_bytes)
        assert INPUT_PEAK.search(direct).group(1) == INPUT_PEAK.search(piped).group(1)
        assert LUFS.search(direct).group(1) == LUFS.search(piped).group(1)
    assert source.read_bytes() == source_bytes
    # Fs/4 signal has a substantial inter-sample peak; cap must attenuate it.
    fixture(source, amplitude=.8, square=True)
    _, text = run('-y', '-p', '-n', '--true-peak', '--target-lufs', '-5',
                  '--peak-ceiling', '-2', '--output', folder / 'limited.wav', source)
    assert float(GAIN.search(text).group(1)) < -1., text
    actual_peak = float(re.search(r'Output true peak:\s*([+-]?[\d.]+)', text).group(1))
    assert actual_peak <= -2., text
    # Self-meter equality alone cannot expose interpolation under-read.
    for rate, frequency, phase in ((48000, .4, 0.), (96000, .25, math.pi / 8),
                                   (192000, .25, math.pi / 4)):
        count = max(8192, rate // 10)
        values = [round(32767 * .7 * min(1., i / 512., (count - 1 - i) / 512.) *
                        math.sin(2 * math.pi * frequency * i + phase)) for i in range(count)]
        with wave.open(str(source), 'wb') as f:
            f.setparams((1, 2, rate, 0, 'NONE', 'PCM'))
            f.writeframes(struct.pack(f'<{count}h', *values))
        target = folder / f'independent-peak-{rate}.wav'
        run('-y', '-p', '-n', '-g', '12', '--peak-ceiling', '-1', '-b', '5',
            '--output', target, source)
        pcm = audio(target.read_bytes())
        values = struct.unpack(f'<{len(pcm) // 4}f', pcm)
        assert reconstructed_peak(values) <= 10 ** (-1 / 20), rate
        fast_target = folder / f'fast-peak-{rate}.wav'
        _, fast_text = run('-y', '-p', '-n', '-g', '12', '--peak-ceiling', '-1',
                           '--true-peak-mode', 'fast', '-b', '5', '--output', fast_target, source)
        assert float(re.search(r'Output true peak:\s*([+-]?[\d.]+)', fast_text).group(1)) <= -1.
        # Compare a file calculation with the apply path and corrected-DC path.
        for dc_flags in (['-p'], []):
            _, fast = run('--true-peak-mode', 'fast', '--true-peak', *dc_flags, source)
            _, precise = run('--true-peak-mode', 'precise', *dc_flags, source)
            if rate == 48000:
                assert float(INPUT_PEAK.search(precise).group(1)) > float(INPUT_PEAK.search(fast).group(1)) + .2
            assert INPUT_PEAK.search(fast).group(1) == INPUT_PEAK.search(fast_text).group(1)
    # Album relative gating pools block energies, rather than averaging LUFS.
    fixture(source, amplitude=.2)
    fixture(second, amplitude=.02)
    _, loud = run('--lufs', '-p', source)
    other = second_dir / 'other.wav'; other.write_bytes(second.read_bytes())
    _, album = run('-y', '-a', '--lufs', '--true-peak-mode', 'fast', '-p', '--output-dir', folder / 'album', source, other)
    gains = list(map(float, GAIN.findall(album)))
    loud_lufs = float(LUFS.search(loud).group(1))
    assert len(gains) == 2 and gains[0] == gains[1]
    assert abs(gains[0] - (-23. - loud_lufs)) < .15, album
    # High-rate analysis preserves the original file's sample rate and frame count.
    for rate in (176400, 192000):
        fixture(source, rate=rate)
        high = folder / f'{rate}.wav'
        run('-y', '--output', high, source)
        with wave.open(str(high)) as f:
            assert f.getframerate() == rate and f.getnframes() == rate
        run('--lufs', '--true-peak', source)
    # Rate-aware dither is explicit, deterministic per process and supports high rates.
    for rate in (8000, 44100, 96000, 192000):
        fixture(source, rate=rate, amplitude=.0001)
        for dither in (1, 2, 3, 4):
            target = folder / f'rate-{rate}-{dither}.wav'
            run('-y', '--output', target, '-d', dither, '--dither-profile', 'rate', source)
            with wave.open(str(target)) as f:
                assert f.getframerate() == rate and f.getnframes() == rate
    fixture(source, amplitude=0.)
    run('-y', '--lufs', '--true-peak', '--output', folder / 'silence.wav', source)
    fixture(source, seconds=.1)
    run('--lufs', source, ok=False)
    fixture(source, rate=50000)
    _, error = run('--lufs', source, ok=False)
    assert 'Unsupported sample rate for LUFS: 50000 Hz' in error, error
    for flags in (('--lufs', '--fast'), ('--true-peak', '--undo-gain'),
                  ('--true-peak-mode', 'turbo'), ('--true-peak-mode', ''),
                  ('--true-peak-mode', 'fast', '--fast'), ('--true-peak-mode', 'fast', '--undo-gain'),
                  ('--target-lufs', 'nan'), ('--peak-ceiling', 'inf'),
                  ('--dither-profile', 'rate'), ('--output', target),
                  ('-y', '--output', target, '--stdout')):
        run(*flags, source, ok=False)
print('New feature regressions passed: safe output paths, recursive layout, LUFS/album gating, true-peak ceilings, high rates and rate dither')

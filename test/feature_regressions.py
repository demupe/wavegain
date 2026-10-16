"""CLI contracts for separate output, LUFS/true peak, high rates and rate dither."""
import math
import os
from pathlib import Path
import re
import struct
import stat
import subprocess
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'wavegain'
GAIN = re.compile(r'Applying Gain of\s*([+-]?[\d.]+) dB')
LUFS = re.compile(r'Integrated loudness:\s*([+-]?[\d.]+) LUFS')


def run(*flags, ok=True, input_data=None):
    r = subprocess.run([str(BINARY), *map(str, flags)], input=input_data, capture_output=True, timeout=30)
    text = r.stderr.decode(errors='replace')
    assert 'Sanitizer' not in text and 'runtime error:' not in text, text
    assert (r.returncode == 0) == ok, text
    return r, text


def fixture(path, rate=48000, seconds=1., amplitude=.1, dc=0., square=False):
    count = round(rate * seconds)
    values = [round(32767 * (dc + amplitude * ((1 if i % 4 < 2 else -1) if square else
                                            math.sin(2 * math.pi * 1000 * i / rate)))) for i in range(count)]
    with wave.open(str(path), 'wb') as f:
        f.setparams((1, 2, rate, 0, 'NONE', 'PCM'))
        f.writeframes(struct.pack(f'<{count}h', *values))


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
    # Fs/4 signal has a substantial inter-sample peak; cap must attenuate it.
    fixture(source, amplitude=.8, square=True)
    _, text = run('-y', '-p', '-n', '--true-peak', '--target-lufs', '-5',
                  '--peak-ceiling', '-2', '--output', folder / 'limited.wav', source)
    assert float(GAIN.search(text).group(1)) < -1., text
    actual_peak = float(re.search(r'Output true peak:\s*([+-]?[\d.]+)', text).group(1))
    assert actual_peak <= -2., text
    # Album relative gating pools block energies, rather than averaging LUFS.
    fixture(source, amplitude=.2)
    fixture(second, amplitude=.02)
    _, loud = run('--lufs', '-p', source)
    other = second_dir / 'other.wav'; other.write_bytes(second.read_bytes())
    _, album = run('-y', '-a', '--lufs', '-p', '--output-dir', folder / 'album', source, other)
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
                  ('--target-lufs', 'nan'), ('--peak-ceiling', 'inf'),
                  ('--dither-profile', 'rate'), ('--output', target),
                  ('-y', '--output', target, '--stdout')):
        run(*flags, source, ok=False)
print('New feature regressions passed: safe output paths, recursive layout, LUFS/album gating, true-peak ceilings, high rates and rate dither')

"""Audio correctness across file formats, channel layouts and partial blocks."""
import math
import random
import re
from pathlib import Path
import struct
import tempfile
import wave
from audio_fixtures import wav
from test_support import parse_binary, run_cli

BINARY = parse_binary(__doc__)


def fixture(path, values, bits=16, channels=1, gain=None, rate=44100):
    pcm = b''.join(bytes([v + 128]) if bits == 8 else
                   v.to_bytes(bits // 8, 'little', signed=True) for v in values)
    block = channels * (bits // 8)
    format_bytes = struct.pack('<HHIIHH', 1, channels, rate, rate * block, block, bits)
    path.write_bytes(wav(pcm, format_bytes, gain=gain))


def samples(path):
    with wave.open(str(path), 'rb') as f:
        width = f.getsampwidth()
        data = f.readframes(f.getnframes())
    return [data[i] - 128 if width == 1 else
            int.from_bytes(data[i:i + width], 'little', signed=True)
            for i in range(0, len(data), width)]


with tempfile.TemporaryDirectory(prefix='wavegain-precision-') as temp:
    folder = Path(temp)
    # A preceding low-bit-depth file must not inject its last random value
    # into the one-LSB noise of a later 24/32-bit silent file.
    for bits in (24, 32):
        first, silent = folder / f'first-{bits}.wav', folder / f'silent-{bits}.wav'
        fixture(first, [round(20 * math.sin(i * .13)) for i in range(44100 * 2)],
                bits=8, channels=2)
        fixture(silent, [0] * (44100 * 2), bits=bits, channels=2)
        out = folder / f'batch-{bits}'
        run_cli(BINARY, ['-y', '-p', '-d', '1', '--output-dir', out, first, silent])
        noise = samples(out / silent.name)
        assert max(map(abs, noise)) <= 1, 'Cross-file dither transient'
        assert any(noise), 'Missing high-bit-depth dither'

    # Undo a stored unit scale to exercise quantization with a known exact
    # gain and no analysis-dependent scaling or DC removal. Quantization noise
    # must stay small even when a stereo tail follows populated shaper history.
    for tail in (1, 7, 15, 17, 31):
        values = [round(3200 * math.sin(i * .123 + ch * .37))
                  for i in range(16384 + tail) for ch in range(2)]
        source = folder / f'shaped-{tail}.wav'
        fixture(source, values, channels=2, gain=1.)
        for mode in (2, 3, 4):
            target = folder / f'out-shaped-{tail}-{mode}.wav'
            run_cli(BINARY, ['-y', '--undo-gain', '--force', '-d', str(mode),
                             '--output', target, source])
            actual = samples(target)
            assert len(actual) == len(values)
            # A generous 64-LSB budget isolates history discontinuities from normal
            # shaped quantization noise; it is not a universal perceptual limit.
            error = max(abs(a - b) for a, b in zip(actual, values))
            assert error < 64, (tail, mode, error)

    # Every actual channel contains the same DC value. Missing right-channel
    # samples in mono files cannot dilute the stereo channel's correction.
    mono, stereo = folder / 'mono.wav', folder / 'stereo.wav'
    fixture(mono, [8192] * 22050)
    fixture(stereo, [8192] * (44100 * 2), channels=2)
    for order, inputs in enumerate(((mono, stereo), (stereo, mono))):
        out = folder / f'album-{order}'
        run_cli(BINARY, ['-y', '-q', '--output-dir', out, *inputs])
        for source in inputs:
            assert not any(samples(out / source.name)), 'Incomplete album DC removal'

    # Signed PCM includes the full negative endpoint. A unit-scale round trip
    # must preserve it through both the plain and general conversion paths.
    values = [-2147483648, -2147483647, -1, 0, 1, 2147483646, 2147483647]
    source = folder / 'endpoint.wav'
    fixture(source, values, bits=32, gain=1.)
    for dither in (0, 1):
        for explicit in (False, True):
            target = folder / f'endpoint-{dither}-{explicit}.wav'
            run_cli(BINARY, ['-y', '--undo-gain', '--force', '-d', str(dither),
                             *(['-b', '4'] if explicit else []), '--output', target, source])
            actual = samples(target)
            if not dither:
                assert actual == values, '32-bit PCM endpoint changed at unit gain'
            else:
                assert all(abs(a-b) <= 1 for a,b in zip(actual, values)), '32-bit dither exceeds one LSB'

    # A sub-16-bit DC correction and a small manual adjustment must have the
    # same effect in place and with explicit output.
    source, inplace, target = folder/'small.wav', folder/'small-inplace.wav', folder/'small-out.wav'
    fixture(source, [round(.1*8388608*math.sin(2*math.pi*997*i/44100))+128 for i in range(44100)], bits=24)
    analysis = run_cli(BINARY, [source]).stderr.decode(errors='replace')
    gain = float(re.search(r'\n\s*([+-][\d.]+) dB\s*\|', analysis)[1])
    manual = .05-gain
    inplace.write_bytes(source.read_bytes())
    run_cli(BINARY, ['-y', '-g', str(manual), inplace])
    run_cli(BINARY, ['-y', '-g', str(manual), '--output', target, source])
    assert samples(inplace) == samples(target)
    assert abs(sum(samples(target))/44100) < .01

    # Equal-duration files at different rates must contribute equally to ADC.
    a, b = folder/'rate-a.wav', folder/'rate-b.wav'
    fixture(a, [328]*48000, rate=48000)
    fixture(b, [983]*192000, rate=192000)
    out = folder/'mixed-rates'
    run_cli(BINARY, ['-y', '-a', '-q', '--output-dir', out, a, b])
    av, bv = samples(out/a.name), samples(out/b.name)
    assert abs(sum(av)/len(av) + sum(bv)/len(bv)) <= 1

    # 400 ms is exactly 4410 frames at 11025 Hz.
    for n in (4409, 4410, 4411):
        source=folder/f'window-{n}.wav'
        fixture(source, [round(3200*math.sin(2*math.pi*997*i/11025)) for i in range(n)], rate=11025)
        run_cli(BINARY, ['--lufs', '-p', source], success=n>=4410)

    # Real-file exercise of compensated DC accumulation beyond the point at
    # which a 32-bit constant loses its low bit in a naive running double sum.
    source, target = folder/'long-dc.wav', folder/'long-dc-out.wav'
    with wave.open(str(source), 'wb') as f:
        f.setparams((1,4,192000,0,'NONE','PCM'))
        block=struct.pack('<i',1073741825)*16384
        frames=9000000
        while frames:
            n=min(frames,16384);f.writeframesraw(block[:n*4]);frames-=n
    run_cli(BINARY, ['-y', '--output', target, source])
    with wave.open(str(target),'rb') as f:
        while data:=f.readframes(16384):
            assert not any(data), 'Residual DC from inaccurate long-file mean'

    # Selected extended reconstruction must also be used on quantized output.
    rng=random.Random(20261019)
    pulse=[rng.uniform(-.5,.5) if 40<=i<88 else 0. for i in range(128)]
    source,target=folder/'transient.wav',folder/'transient-out.wav'
    fixture(source,[0]*512+[round(v*32768) for v in pulse]+[0]*24000,rate=48000)
    result=run_cli(BINARY,['-y','-p','--lufs','--noclip','--true-peak-mode','extended','--output',target,source])
    assert b'Output true peak:' in result.stderr
    data=samples(target);active=[i for i,v in enumerate(data) if v]
    values=[v/32768 for v in data[min(active):max(active)+1]]
    # Independent finite zero-extended sinc reconstruction on a dense grid.
    peak=0.
    for step in range(-32*len(values),64*len(values)):
        t=step/32
        value=math.fsum(x*(math.sin(math.pi*(t-i))/(math.pi*(t-i)) if t!=i else 1.)
                        for i,x in enumerate(values))
        peak=max(peak,abs(value))
    assert 20*math.log10(peak) <= -1., 'Extended output exceeds ideal-sinc fixture ceiling'

print('Precision regressions: dither reset/tail, mixed-channel DC and PCM endpoints passed')

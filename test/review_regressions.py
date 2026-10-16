"""Regressions for the October 2026 safety review (stdlib only)."""
import argparse
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=ROOT / 'wavegain')
args = parser.parse_args()
BINARY = args.binary.resolve()
RATE = 44100
FRAMES = 4411  # odd mono data length after conversion to 8/24 bit
PCM = [round(6000 * math.sin(i * .2)) for i in range(FRAMES)]
GAIN_ENDIAN = '>' if sys.platform == 'darwin' else '<'


def chunk(tag, payload, endian='<'):
    return tag + struct.pack(endian + 'I', len(payload)) + payload + bytes(len(payload) & 1)


def wav(*, before=b'', middle=b'', tail=b'', bits=16, values=None):
    values = PCM if values is None else values
    width = bits // 8
    if bits == 8:
        data = bytes(values)
    else:
        data = b''.join(v.to_bytes(width, 'little', signed=True) for v in values)
    fmt = struct.pack('<HHIIHH', 1, 1, RATE, RATE * width, width, bits)
    body = b'WAVE' + before + chunk(b'fmt ', fmt) + middle + chunk(b'data', data) + tail
    return b'RIFF' + struct.pack('<I', len(body)) + body


def aiff(bits=16, *, aifc=None, offset=0):
    data = (bytes(FRAMES) if bits == 8 else struct.pack(
        ('<' if aifc == b'sowt' else '>') + f'{FRAMES}h', *PCM))
    comm = struct.pack('>hIh', 1, FRAMES, bits) + bytes.fromhex('400eac44000000000000')
    if aifc:
        comm += aifc + b'\0\0'
    body = ((b'AIFC' if aifc else b'AIFF') + chunk(b'COMM', comm, '>')
            + chunk(b'SSND', struct.pack('>II', offset, 0) + bytes(offset) + data, '>'))
    return b'FORM' + struct.pack('>I', len(body)) + body


def run(folder, *flags, success=True, input_data=None):
    result = subprocess.run([str(BINARY), *map(str, flags)], cwd=folder,
                            input=input_data, capture_output=True, timeout=20)
    diagnostic = result.stderr.decode(errors='replace')
    assert 'Sanitizer' not in diagnostic and 'runtime error:' not in diagnostic, diagnostic
    assert (result.returncode == 0) == success, diagnostic
    return result


def chunks(data):
    assert data[:4] == b'RIFF' and data[8:12] == b'WAVE'
    assert struct.unpack_from('<I', data, 4)[0] == len(data) - 8
    found = {}
    pos = 12
    while pos < len(data):
        assert pos + 8 <= len(data)
        tag, size = struct.unpack_from('<4sI', data, pos)
        assert pos + 8 + size + (size & 1) <= len(data)
        assert tag not in found, tag
        found[tag] = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    return found


with tempfile.TemporaryDirectory(prefix='wavegain-review-') as temp:
    folder = Path(temp)
    path = folder / 'input.wav'
    # Valid opaque metadata must survive marker-like payloads and odd lengths.
    for value in (0, 0x10000000):
        metadata = b'fmt ' + struct.pack('<I', value) + b'RIFFgain'
        original = wav(before=chunk(b'JUNK', metadata), middle=chunk(b'LIST', b'xyz'),
                       tail=chunk(b'cue ', b'opaque'))
        for bits in (1, 2, 3, 4, 5):
            path.write_bytes(original)
            run(folder, '-y', '-w', '-b', bits, path)
            result = chunks(path.read_bytes())
            assert result[b'JUNK'] == metadata and result[b'LIST'] == b'xyz'
            assert result[b'cue '] == b'opaque' and len(result[b'gain']) == 8
            assert len(result[b'data']) == FRAMES * (1, 2, 3, 4, 4)[bits - 1]
            # A deliberate skip succeeds without rewriting, and undo works.
            before = path.read_bytes()
            run(folder, '-y', path)
            assert path.read_bytes() == before
            run(folder, '--undo-gain', path)
            chunks(path.read_bytes())
    # Every supported input width includes negative endpoint samples.
    for bits in (8, 16, 24, 32):
        endpoints = ([0, 128, 255] if bits == 8 else
                     [-(1 << (bits - 1)), -1, 0, (1 << (bits - 1)) - 1])
        path.write_bytes(wav(bits=bits, values=(endpoints * (FRAMES // len(endpoints) + 1))[:FRAMES]))
        run(folder, '-y', '-p', path)
        chunks(path.read_bytes())
    # AIFF and both uncompressed AIFC byte orders support WAV conversion.
    reference_audio = {}
    for bits in (1, 2, 3, 4, 5, 6):
        path.write_bytes(wav())
        run(folder, '-y', '-b', bits, path)
        data = path.read_bytes()
        reference_audio[bits] = data[54:] if bits == 6 else chunks(data)[b'data']
    for codec in (None, b'NONE', b'sowt'):
        original = aiff(aifc=codec, offset=3)
        for bits in (1, 2, 3, 4, 5, 6):
            path.write_bytes(original)
            run(folder, '-y', '-b', bits, path)
            output = path.read_bytes()
            if bits == 6:
                assert output[:4] == b'FORM' and output[8:12] == b'AIFF'
                assert struct.unpack_from('>I', output, 4)[0] == len(output) - 8
                assert len(output) == 54 + FRAMES * 2
                assert output[54:] == reference_audio[bits]
            else:
                result = chunks(output)
                assert len(result[b'data']) == FRAMES * (1, 2, 3, 4, 4)[bits - 1]
                assert result[b'data'] == reference_audio[bits]
        path.write_bytes(original)
        output = run(folder, '-y', '-w', '-o', '-b', 2, path).stdout
        assert len(chunks(output)[b'data']) == FRAMES * 2
        assert path.read_bytes() == original
    # Invalid AIFF input and gain payloads fail safely with original bytes intact.
    invalid = [aiff(8),
               b'FORM' + struct.pack('>I', 0xffffffff) + b'AIFFCOMM' + struct.pack('>I', 0x40000000),
               aiff()[:-2],
               wav(middle=chunk(b'gain', b'')),
               wav(middle=chunk(b'gain', bytes(7))),
               wav(middle=chunk(b'gain', bytes(9)))]
    for scale in (0., -1., float('inf'), float('nan'), 5e-324):
        invalid.append(wav(middle=chunk(b'gain', struct.pack(GAIN_ENDIAN + 'd', scale))))
    for original in invalid:
        for flags in (('-y',), ('--undo-gain',)):
            path.write_bytes(original)
            run(folder, *flags, path, success=False)
            assert path.read_bytes() == original
            assert not list(folder.glob('*.tmp_*'))
    # A large valid exponent no longer produces an oversized integer shift.
    path.write_bytes(wav(middle=chunk(b'gain', struct.pack(GAIN_ENDIAN + 'd', 2. ** 40))))
    run(folder, '--undo-gain', path)
    assert struct.unpack(GAIN_ENDIAN + 'd', chunks(path.read_bytes())[b'gain'])[0] == 1.
    # Extreme finite undo gains saturate rather than overflowing the quantizer.
    for dither in range(5):
        path.write_bytes(wav(middle=chunk(b'gain', struct.pack(GAIN_ENDIAN + 'd', 1e-300))))
        run(folder, '--undo-gain', '-d', dither, path)
        assert len(chunks(path.read_bytes())[b'data']) == FRAMES * 2
    # The PCM starts after, rather than inside, the declared extended fmt payload.
    fmt = struct.pack('<HHIIHH', 1, 1, RATE, RATE * 2, 2, 16) + bytes(84)
    body = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'data', struct.pack(f'<{FRAMES}h', *PCM))
    path.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)
    run(folder, '-y', path)
    assert len(chunks(path.read_bytes())[b'data']) == FRAMES * 2
    # Long formatted arguments are logged in full, and overlong log paths reject.
    path.write_bytes(wav())
    log = folder / 'log.txt'
    run(folder, '-f', log, '-g', 'x' * 2000, path)
    assert 'x' * 2000 in log.read_text()
    run(folder, '-f', 'x' * 2000, path, success=False)
    # Partial failures remain visible while later valid files are processed.
    bad = folder / 'bad.wav'
    bad.write_bytes(b'bad input')
    path.write_bytes(wav())
    run(folder, '-y', bad, path, success=False)
    chunks(path.read_bytes())
    # Stdin is bounded: declared finite audio sizes must not hide truncation.
    run(folder, '-c', '-', input_data=wav()[:-100], success=False)
    # A zero data size cannot cause trailing metadata to become audio.
    path.write_bytes(wav(values=[], tail=chunk(b'LIST', bytes(10000))))
    before = path.read_bytes()
    run(folder, '-y', path, success=False)
    assert path.read_bytes() == before
    # POSIX recursive symlink cycles do not prevent visiting real files.
    if sys.platform != 'win32':
        directory = folder / 'recursive'
        directory.mkdir()
        (directory / 'loop').symlink_to(directory, target_is_directory=True)
        (directory / 'valid.wav').write_bytes(wav())
        run(folder, '-z', '-c', directory)

print('Review regressions passed: RIFF metadata/padding, AIFF conversion/rejection, gain validation, PCM endpoints, logging and failure status')

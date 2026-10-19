"""Small structural WAV/Wave64 fixtures shared by safety regressions."""
import struct
import sys

GUID = bytes.fromhex('0100000000001000800000aa00389b71')
RIFF64 = bytes.fromhex('726966662e91cf11a5d628db04c10000')
WAVE64 = bytes.fromhex('77617665f3acd3118cd100c04f8edb8a')
GAIN64 = bytes.fromhex('48d817e2be96c04ab56a8b46289c71e0')


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes(len(data) % 2)


def fmt(bits=16, channels=2, floating=False, valid=None, cb=22, extra=b''):
    block = channels * (bits // 8)
    data = struct.pack('<HHIIHH', 0xfffe if valid is not None else 3 if floating else 1,
                       channels, 44100, 44100 * block, block, bits)
    if valid is not None:
        guid = bytes([3]) + GUID[1:] if floating else GUID
        data += struct.pack('<HHI', cb, valid, 3 if channels == 2 else 4) + guid + extra
    return data


def wav(pcm, format_bytes=fmt(), gain=None):
    body = b'WAVE' + chunk(b'fmt ', format_bytes)
    if gain is not None:
        body += chunk(b'gain', struct.pack('>d' if sys.platform == 'darwin' else '<d', gain))
    body += chunk(b'data', pcm)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def chunk64(guid, data):
    result = guid + struct.pack('<Q', len(data) + 24) + data
    return result + bytes(-len(result) % 8)


def w64(pcm, scale=None, tail=False, format_bytes=fmt()):
    gain = chunk64(GAIN64, struct.pack('<d', scale)) if scale is not None else b''
    body = chunk64(b'fmt ' + WAVE64[4:], format_bytes)
    body += (b'' if tail else gain) + chunk64(b'data' + WAVE64[4:], pcm) + (gain if tail else b'')
    return RIFF64 + struct.pack('<Q', 40 + len(body)) + WAVE64 + body


def audio(data):
    pos = 12
    while pos + 8 <= len(data):
        size = struct.unpack_from('<I', data, pos + 4)[0]
        if data[pos:pos + 4] == b'data':
            return data[pos + 8:pos + 8 + size]
        pos += 8 + size + size % 2
    raise AssertionError('Missing output audio')

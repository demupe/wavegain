"""Reproduce the 2026-10-18 audit findings on synthetic files; not a passing gate.

All rewrites are restricted to fixtures under .benchmark-windows/audit-20261018.
The JSON records observed behavior, including known failures, without changing
production code. Pass a separately built UBSan executable to --ubsan if desired.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
folder = ROOT / '.benchmark-windows' / 'audit-20261018'
folder.mkdir(parents=True, exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=ROOT / 'wavegain.exe')
parser.add_argument('--ubsan', type=Path)
args = parser.parse_args()
BIN = args.binary.resolve()
report = {}

def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes(len(data) % 2)

def wav(data, channels=2, bits=16, floating=False, extension=None):
    block = channels * (bits // 8)
    fmt = struct.pack('<HHIIHH', 0xfffe if extension else 3 if floating else 1,
                      channels, 44100, 44100 * block, block, bits)
    if extension:
        fmt += struct.pack('<HHI', 22, extension, 3) + bytes.fromhex('0100000000001000800000aa00389b71')
    body = b'WAVE' + chunk(b'fmt ', fmt) + chunk(b'data', data)
    return b'RIFF' + struct.pack('<I', len(body)) + body

def run(label, path, args, binary=BIN):
    before = path.read_bytes()
    r = subprocess.run([str(binary), *args, str(path)], capture_output=True, timeout=20)
    report[label] = {'returncode': r.returncode,
                     'input_changed': path.read_bytes() != before,
                     'stderr': r.stderr.decode('utf-8', errors='replace')[-2000:]}
    print(label, r.returncode, 'changed', path.read_bytes() != before, flush=True)
    return r

pcm = b''.join(struct.pack('<hh', round(5000 * math.sin(i * .14)), round(4000 * math.cos(i * .17)))
               for i in range(22050))
path = folder / 'partial-frame.wav'
path.write_bytes(wav(pcm + b'\x7f'))
run('partial_frame', path, ['-y', '--force', '-g', '-3'])
report['partial_frame']['input_data_bytes'] = len(pcm) + 1
report['partial_frame']['output_data_bytes'] = struct.unpack_from('<I', path.read_bytes(), 40)[0]

path = folder / 'extensible-invalid-validbits.wav'
path.write_bytes(wav(pcm, extension=24))
run('invalid_validbits_24_in_16', path, ['-c'])

path = folder / 'large-dc.wav'
path.write_bytes(wav(struct.pack('<f', 100000.) * 44100, channels=1, bits=32, floating=True))
run('large_album_dc', path, ['-c', '-q'])
if args.ubsan:
    run('large_album_dc_ubsan', path, ['-c', '-q'], args.ubsan.resolve())

riff = bytes.fromhex('726966662e91cf11a5d628db04c10000')
wave = bytes.fromhex('77617665f3acd3118cd100c04f8edb8a')
gain = bytes.fromhex('48d817e2be96c04ab56a8b46289c71e0')
def chunk64(guid, data):
    result = guid + struct.pack('<Q', 24 + len(data)) + data
    return result + bytes(-len(result) % 8)
fmt = struct.pack('<HHIIHH', 1, 2, 44100, 176400, 4, 16)
body = chunk64(b'fmt ' + wave[4:], fmt) + chunk64(gain, struct.pack('<d', 5e-324))
body += chunk64(b'data' + wave[4:], pcm)
path = folder / 'tiny-gain.w64'
path.write_bytes(riff + struct.pack('<Q', 40 + len(body)) + wave + body)
run('wave64_subnormal_gain_calculate', path, ['-c'])
run('wave64_subnormal_gain_undo', path, ['--undo-gain'])
report['wave64_subnormal_gain_undo']['output_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
output = path.read_bytes()
data_start = output.index(b'data' + wave[4:]) + 24
values = struct.unpack('<' + 'h' * (len(pcm) // 2), output[data_start:data_start + len(pcm)])
report['wave64_subnormal_gain_undo']['saturated_samples'] = sum(abs(v) >= 32767 for v in values)
report['wave64_subnormal_gain_undo']['total_samples'] = len(values)

single = folder / 'single.wav'
duplicate = folder / 'duplicate.wav'
single.write_bytes(wav(pcm))
duplicate.write_bytes(wav(pcm))
run('single_input', single, ['-y', '-p', '-n', '--force', '-g', '-6'])
run('duplicate_input', duplicate, ['-y', '-p', '-n', '--force', '-g', '-6', str(duplicate)])
report['duplicate_input']['matches_single_apply'] = single.read_bytes() == duplicate.read_bytes()
for label, path in [('single_input', single), ('duplicate_input', duplicate)]:
    values = struct.unpack('<' + 'h' * (len(pcm) // 2), path.read_bytes()[44:44 + len(pcm)])
    report[label]['sample_peak'] = max(abs(v) for v in values)

(folder / 'results.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')

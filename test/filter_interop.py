# SPDX-License-Identifier: LGPL-2.1-or-later
"""Independent 88.2 kHz sanity checks with FFmpeg, including conversion/undo."""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def run(args):
    return subprocess.run(list(map(str,args)),check=True,capture_output=True,text=True)

def fixture(path, tone, channels, seconds=4):
    rate = 88200
    payload = bytearray()
    for i in range(rate*seconds):
        for ch in range(channels):
            value = .2*math.sin(2*math.pi*tone*(1+.17*ch)*i/rate)
            payload.extend(struct.pack('<f',value))
    fmt = struct.pack('<HHIIHH',3,channels,rate,rate*channels*4,channels*4,32)
    path.write_bytes(b'RIFF'+struct.pack('<I',36+len(payload))+b'WAVEfmt '+struct.pack('<I',16)+fmt+
                     b'data'+struct.pack('<I',len(payload))+payload)

def verify(path, channels, bits):
    info = json.loads(run(['ffprobe','-v','error','-show_entries',
                          'stream=codec_name,sample_rate,channels,duration','-of','json',path]).stdout)['streams'][0]
    assert int(info['sample_rate']) == 88200 and info['channels'] == channels
    assert info['codec_name'] == ['pcm_u8','pcm_s16le','pcm_s24le','pcm_s32le','pcm_f32le'][bits-1]
    assert abs(float(info['duration'])-4) < 1e-6
    raw = subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-f','f32le','-'])
    assert len(raw) == 88200*4*channels*4
    values = [x[0] for x in struct.iter_unpack('<f',raw)]
    assert all(math.isfinite(x) for x in values)
    return values

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=ROOT/'wavegain')
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    report = {'ffmpeg':run(['ffmpeg','-version']).stdout.splitlines()[0],'cases':[]}
    with tempfile.TemporaryDirectory(prefix='wavegain-88200-interop-') as temp:
        folder = Path(temp)
        for channels in (1,2):
            for tone in (80,440,4000,16000):
                source = folder/'tone.wav'
                fixture(source,tone,channels)
                original = source.read_bytes()
                expected = [x[0] for x in struct.iter_unpack('<f',original[44:])]
                wg = run([args.binary,'-c','-n','-p',source])
                match = re.search(r'([+-]?[\d.]+) dB\s*\|',wg.stderr)
                assert match, wg.stderr
                gain = float(match[1]); assert math.isfinite(gain)
                filters = 'pan=stereo|c0=c0|c1=c0,replaygain' if channels == 1 else 'replaygain'
                ff = run(['ffmpeg','-nostdin','-hide_banner','-i',source,'-af',filters,'-f','null','-'])
                reference = float(re.search(r'track_gain = ([+-]?[\d.]+) dB',ff.stderr)[1])
                # FFmpeg stores float history, normalizes RMS differently, and
                # uses 64.54 instead of WaveGain's 64.82 calibration. Approximate
                # agreement is an independent sanity check, never a bit oracle.
                difference = gain-reference
                assert abs(difference) <= .15,(channels,tone,gain,reference)
                report['cases'].append({'channels':channels,'tone_hz':tone,
                                        'wavegain_db':gain,'ffmpeg_db':reference,'difference_db':difference})
                for container in ('wav','w64'):
                    path = folder/('convert.'+container)
                    if container == 'wav': path.write_bytes(original)
                    else: run(['ffmpeg','-v','error','-y','-i',source,'-c:a','pcm_f32le','-f','w64',path])
                    before = path.read_bytes()
                    for bits in range(1,6):
                        path.write_bytes(before)
                        run([args.binary,'-y','-w','-p','-b',bits,path]); verify(path,channels,bits)
                        run([args.binary,'--undo-gain','-p',path])
                        restored = verify(path,channels,bits)
                        width = [8,16,24,32,32][bits-1]
                        tolerance = max(2/(1 << (width-1)),1e-6)
                        assert max(abs(x-y) for x,y in zip(expected,restored)) <= tolerance
        print('88.2 kHz FFmpeg comparison and WAV/Wave64 conversion/undo passed: mono/stereo, four tones, five output formats.')
    if args.output: args.output.write_text(json.dumps(report,indent=2)+'\n')

if __name__ == '__main__': main()

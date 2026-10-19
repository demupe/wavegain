"""Compare two native CLIs with warm caches and exact output-byte checks.

Build both executables first. Input restoration and hashing are outside the
timer. Results include process launch and file I/O, not forced storage flushes.
No claim about real music or cold-cache throughput follows from this corpus.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile
import time
import wave


def fixture(path, rate, bits, seconds):
    rng = random.Random(20261017)
    pcm = bytearray()
    for i in range(rate):
        for ch in range(2):
            sample = .2 * math.sin(2 * math.pi * (997 + ch * 751) * i / rate)
            sample += .08 * math.sin(2 * math.pi * 73 * i / rate) + rng.uniform(-.03, .03) + .003
            pcm.extend(round(sample * (1 << (bits - 1))).to_bytes(bits // 8, 'little', signed=True))
    with wave.open(str(path), 'wb') as out:
        out.setparams((2, bits // 8, rate, 0, 'NONE', 'not compressed'))
        for _ in range(seconds):
            out.writeframesraw(pcm)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--input', type=Path, action='append', help='Use existing WAVs instead of synthetic inputs')
    parser.add_argument('--seconds', type=int, default=180)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.runs < 1 or args.seconds < 1:
        parser.error('runs and seconds must be positive')
    binaries = {'reference': args.reference.resolve(), 'candidate': args.candidate.resolve()}
    report = {'platform': platform.platform(), 'processor': os.environ.get('PROCESSOR_IDENTIFIER', platform.processor()),
              'method': 'One warm-up, alternating order, median wall seconds; restoration/hashing outside timer; warm OS cache',
              'binaries': {k: {'path': str(v), 'sha256': hashlib.sha256(v.read_bytes()).hexdigest()} for k, v in binaries.items()},
              'runs': args.runs, 'cases': []}
    with tempfile.TemporaryDirectory(prefix='wavegain-native-benchmark-') as temp:
        folder = Path(temp)
        inputs = [p.resolve() for p in args.input] if args.input else []
        if not inputs:
            for rate, bits in ((44100, 16), (96000, 24), (192000, 24)):
                source = folder / f'{rate}-{bits}.wav'
                fixture(source, rate, bits, args.seconds)
                inputs.append(source)
        for source in inputs:
            with wave.open(str(source), 'rb') as wav:
                info = {'rate': wav.getframerate(), 'channels': wav.getnchannels(), 'bits': wav.getsampwidth() * 8,
                        'frames': wav.getnframes()}
            for mode, option in [('analyze', '-c'), ('apply', '-y')]:
                times = {k: [] for k in binaries}
                hashes = {}
                for iteration in range(args.runs + 1):
                    order = list(binaries) if iteration % 2 else list(reversed(binaries))
                    for name in order:
                        target = source
                        if mode == 'apply':
                            target = folder / f'work-{name}.wav'
                            shutil.copyfile(source, target)
                        started = time.perf_counter()
                        result = subprocess.run([str(binaries[name]), option, str(target)], capture_output=True, timeout=300)
                        elapsed = time.perf_counter() - started
                        assert result.returncode == 0 and b'Error processing' not in result.stderr, result.stderr
                        if iteration:
                            times[name].append(elapsed)
                        if iteration == args.runs and mode == 'apply':
                            hashes[name] = hashlib.sha256(target.read_bytes()).hexdigest()
                assert len(set(hashes.values())) <= 1, (source, hashes)
                medians = {k: statistics.median(v) for k, v in times.items()}
                case = {'input': source.name, **info, 'mode': mode, 'times': times, 'medians': medians,
                        'speedup': medians['reference'] / medians['candidate'], 'output_sha256': hashes}
                report['cases'].append(case)
                args.output.write_text(json.dumps(report, indent=2) + '\n')
                print(f'{source.name} {mode}: {medians}, {case["speedup"]:.3f}x', flush=True)


if __name__ == '__main__':
    main()

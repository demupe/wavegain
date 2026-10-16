"""Exact native libebur128 comparison; optional CPU-only synthetic benchmark."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--cc', default='cc')
p.add_argument('--cflags', default='-O2')
p.add_argument('--benchmark', action='store_true')
p.add_argument('--output', type=Path)
args = p.parse_args()
report = {'platform': platform.platform(), 'architecture': platform.machine(),
          'compiler': subprocess.check_output([*shlex.split(args.cc), '--version'], text=True).splitlines()[0],
          'flags': args.cflags, 'reference': 'libebur128 v1.2.6, native architecture, FP contraction disabled',
          'optimized': 'R128Net-inspired kernels plus stereo block-energy SIMD and peak-only bypass',
          'benchmark': '60 s synthetic stereo 48 kHz (also 192 kHz peak-only), CPU only, five alternating runs, medians'}
with tempfile.TemporaryDirectory(prefix='wavegain-loudness-kernels-') as temp:
    folder = Path(temp)
    binaries = {}
    hashes = {}
    for name, flags in [('reference', ['-DWG_EBUR_REFERENCE']),
                        ('previous', ['-DWG_EBUR_EXTRA_REFERENCE']), ('optimized', [])]:
        binary = folder / name
        subprocess.run([*shlex.split(args.cc), *shlex.split(args.cflags), '-UNDEBUG', *flags,
                        str(ROOT / 'test/loudness_kernels.c'), '-o', str(binary), '-lm'], check=True)
        binaries[name] = binary
        output = subprocess.check_output([str(binary)])
        hashes[name] = hashlib.sha256(output).hexdigest()
    assert len(set(hashes.values())) == 1, hashes
    report['state_sha256'] = hashes
    print('Loudness kernels: 14 rates, mono/stereo, exact block energies and histogram gating, irregular chunks, silence/denormals, edge peaks and peak-only adapter match the reference bit-for-bit.')
    if args.benchmark:
        modes = ('lufs', 'lufs_tp', 'peak_only', 'peak_only_192k')
        samples = {name: {mode: [] for mode in modes} for name in binaries}
        for i in range(5):
            for name in (list(binaries) if i % 2 == 0 else list(reversed(binaries))):
                for line in subprocess.check_output([str(binaries[name]), 'benchmark'], text=True).splitlines():
                    mode, seconds, _ = line.split()
                    samples[name][mode].append(float(seconds))
        report['cpu_seconds'] = samples
        report['median_seconds'] = {name: {mode: statistics.median(v) for mode, v in modes.items()}
                                    for name, modes in samples.items()}
        report['speedup'] = {mode: report['median_seconds']['reference'][mode] /
                            report['median_seconds']['optimized'][mode] for mode in modes}
        report['incremental_speedup'] = {mode: report['median_seconds']['previous'][mode] /
                                        report['median_seconds']['optimized'][mode] for mode in modes}
        print(json.dumps(report['speedup']))
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')

"""Compile scalar/SIMD private-state probes and compare all output bytes."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shlex
import signal
import subprocess
import struct
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]

def check_cli(args, folder):
    """Compare the built CLI with a scalar build using identical build flags."""
    scalar = folder / 'wavegain-scalar'
    subprocess.run([*shlex.split(args.cc), *shlex.split(args.cflags),
                    '-DHAVE_CONFIG_H', '-DWAVEGAIN_FILTER_SIMD=0',
                    *map(str, sorted(ROOT.glob('*.c'))), '-o', str(scalar),
                    *shlex.split(args.ldflags)], check=True)
    cases = []
    for rate in (44100, 48000, 88200):
        for channels in (1, 2):
            original = folder / 'input.wav'
            pcm = bytearray()
            for i in range(rate * 2 + 17):
                for channel in range(channels):
                    sample = (1200 + channel * 900 +
                              4000 * math.sin(2 * math.pi * (997 + channel * 751) * i / rate) +
                              2000 * math.sin(2 * math.pi * 73 * i / rate))
                    pcm.extend(struct.pack('<h', round(sample)))
            with wave.open(str(original), 'wb') as out:
                out.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
                out.writeframes(pcm)
            for options in (['-y'], ['-y', '-p']):
                outputs = []
                for name, executable in [('scalar', scalar), ('candidate', args.cli.resolve())]:
                    target = folder / (name + '.wav')
                    target.write_bytes(original.read_bytes())
                    subprocess.run([str(executable), *options, str(target)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                    outputs.append(target.read_bytes())
                if outputs[0] != outputs[1]:
                    raise AssertionError(f'CLI output differs: {rate} Hz, {channels} channels, {options}')
                cases.append({'rate': rate, 'channels': channels, 'options': options,
                              'sha256': hashlib.sha256(outputs[0]).hexdigest()})
    print('Built CLI matches scalar output bytes: 12 PCM conversion cases.', flush=True)
    return cases

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cflags', default='-O2')
    parser.add_argument('--ldflags', default='-lm')
    parser.add_argument('--cli', type=Path, help='Also verify this built CLI against an equivalent scalar build')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--simd', choices=('auto','on','off'), default='auto',
                        help='Select the paired path in the default build')
    parser.add_argument('--expect-simd', choices=('0','1'),
                        help='Fail if the default build selects a different backend')
    parser.add_argument('--reference-ref', help='Compare the eleven unaffected rates against a prior Git revision '
                        '(expected to match only on targets without FMA; filters no longer fuse multiply-add)')
    args = parser.parse_args()
    report = {'compiler': subprocess.check_output([*shlex.split(args.cc),'--version'],text=True).splitlines()[0],
              'flags': args.cflags, 'ldflags': args.ldflags, 'simd': args.simd, 'runs': {}}
    with tempfile.TemporaryDirectory(prefix='wavegain-filter-tests-') as temp:
        folder = Path(temp)
        default_flags = ([] if args.simd == 'auto' else
                         ['-DWAVEGAIN_FILTER_SIMD='+str(int(args.simd == 'on'))])
        if args.expect_simd is not None:
            default_flags.append('-DFILTER_EXPECT_SIMD='+args.expect_simd)
        for variant, flags in [('scalar',['-DWAVEGAIN_FILTER_SIMD=0']),('default',default_flags)]:
            binary = folder / variant
            subprocess.run([*shlex.split(args.cc), *shlex.split(args.cflags), '-DHAVE_CONFIG_H',
                            '-UNDEBUG', *flags, str(ROOT/'test/filter_regressions.c'),
                            '-o',str(binary),*shlex.split(args.ldflags)],check=True)
            path = folder / (variant+'.bin')
            with path.open('wb') as out:
                result = subprocess.run([str(binary)],stdout=out,stderr=subprocess.PIPE,text=True,check=True)
            print(result.stderr,end='',flush=True)
            report['runs'][variant] = {'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                                      'bytes':path.stat().st_size,'diagnostic':result.stderr}
        report['bitwise_equal'] = report['runs']['scalar']['sha256'] == report['runs']['default']['sha256']
        if not report['bitwise_equal']: raise AssertionError('SIMD and scalar API state/histograms differ')
        macros = subprocess.check_output([*shlex.split(args.cc),*shlex.split(args.cflags),
                                          '-dM','-E','-x','c','-'],input=b'').decode()
        if '__clang__' in macros and '__x86_64__' in macros:
            binary = folder/'legacy-sse2'
            subprocess.run([*shlex.split(args.cc),*shlex.split(args.cflags),'-DHAVE_CONFIG_H',
                            '-DHAVE_SSE2','-fdeclspec','-UNDEBUG',str(ROOT/'test/filter_legacy_sse2.c'),
                            '-o',str(binary),*shlex.split(args.ldflags)],check=True)
            legacy = subprocess.check_output([str(binary)],text=True)
            report['legacy_sse2_diagnostic'] = legacy
            print(legacy,end='')
        if args.reference_ref:
            reports = {}
            for variant in ('prior','current'):
                build = folder / variant
                (build/'test').mkdir(parents=True)
                (build/'test/filter_regressions.c').write_bytes((ROOT/'test/filter_regressions.c').read_bytes())
                for name in ('gain_analysis.c','gain_analysis.h','config.h'):
                    data = (subprocess.check_output(['git','show',f'{args.reference_ref}:{name}'],cwd=ROOT)
                            if variant == 'prior' else (ROOT/name).read_bytes())
                    (build/name).write_bytes(data)
                binary = build/'probe'
                subprocess.run([*shlex.split(args.cc),*shlex.split(args.cflags),'-DHAVE_CONFIG_H',
                                '-UNDEBUG','-DFILTER_COMPATIBILITY_TEST',str(build/'test/filter_regressions.c'),
                                '-o',str(binary),*shlex.split(args.ldflags)],check=True)
                with (build/'output.bin').open('wb') as out:
                    subprocess.run([str(binary)],stdout=out,stderr=subprocess.PIPE,check=True)
                reports[variant] = hashlib.sha256((build/'output.bin').read_bytes()).hexdigest()
            report['reference_ref'] = args.reference_ref
            report['unaffected_rates_sha256'] = reports
            report['unaffected_rates_equal'] = reports['prior'] == reports['current']
            if not report['unaffected_rates_equal']:
                raise AssertionError('Previously supported rates changed. The filters are now evaluated without '
                                     'FMA, so a difference is expected on FMA-capable targets (arm64, '
                                     '-march=x86-64-v3); compare compatibility on a target without FMA')
            print('Eleven unaffected rates match the prior revision byte-for-byte.')
        if args.cli:
            report['cli_cases'] = check_cli(args, folder)
    if args.output: args.output.write_text(json.dumps(report,indent=2)+'\n')
    print('Scalar/default filter API output is byte-for-byte identical.')

if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        if error.stderr:
            print(error.stderr, end='', flush=True)
        if error.returncode == -signal.SIGILL:
            raise SystemExit('Verification failed: this CPU cannot execute the selected target ISA.')
        raise

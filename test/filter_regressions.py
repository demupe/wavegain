# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compile scalar/SIMD private-state probes and compare all output bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cflags', default='-O2')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reference-ref', help='Compare the eleven unaffected rates against a prior Git revision')
    args = parser.parse_args()
    report = {'compiler': subprocess.check_output([*shlex.split(args.cc),'--version'],text=True).splitlines()[0],
              'flags': args.cflags, 'runs': {}}
    with tempfile.TemporaryDirectory(prefix='wavegain-filter-tests-') as temp:
        folder = Path(temp)
        for variant, flags in [('scalar',['-DWAVEGAIN_FILTER_SIMD=0']),('default',[])]:
            binary = folder / variant
            subprocess.run([*shlex.split(args.cc), *shlex.split(args.cflags), '-DHAVE_CONFIG_H',
                            '-UNDEBUG', *flags, str(ROOT/'test/filter_regressions.c'),
                            '-o',str(binary),'-lm'],check=True)
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
                            '-o',str(binary),'-lm'],check=True)
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
                                '-o',str(binary),'-lm'],check=True)
                with (build/'output.bin').open('wb') as out:
                    subprocess.run([str(binary)],stdout=out,stderr=subprocess.PIPE,check=True)
                reports[variant] = hashlib.sha256((build/'output.bin').read_bytes()).hexdigest()
            report['reference_ref'] = args.reference_ref
            report['unaffected_rates_sha256'] = reports
            report['unaffected_rates_equal'] = reports['prior'] == reports['current']
            if not report['unaffected_rates_equal']: raise AssertionError('Previously supported rates changed')
            print('Eleven unaffected rates match the prior revision byte-for-byte.')
    if args.output: args.output.write_text(json.dumps(report,indent=2)+'\n')
    print('Scalar/default filter API output is byte-for-byte identical.')

if __name__ == '__main__': main()

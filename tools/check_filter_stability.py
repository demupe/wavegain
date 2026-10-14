"""Check all IIR denominators using Schur recursion (no third-party modules)."""
import argparse
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
RATES = [96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000]

def tables(text, name):
    return [[list(map(float,row.split(','))) for row in re.findall(r'\{([^{}]+)\}',body)]
            for body in re.findall(r'static const Float_t '+name+r'\[[^=]+?=\s*\{(.*?)\n\};',text,re.S)]

def schur(coefficients):
    # A real polynomial is Schur-stable iff |last/first| < 1 at every
    # reciprocal-polynomial reduction. Coefficients descend from z**order.
    a = coefficients[:]
    reflection = []
    while len(a) > 1:
        ratio = a[-1] / a[0]
        reflection.append(ratio)
        if abs(ratio) >= 1:
            return {'stable': False, 'reflection_coefficients':reflection}
        b = [a[i]-ratio*a[-1-i] for i in range(len(a)-1)]
        a = [v/b[0] for v in b]
    return {'stable':True,'reflection_coefficients':reflection,
            'minimum_reflection_margin':min(1-abs(x) for x in reflection)}

def polynomial(a, x):
    value = 0.
    for coefficient in a: value = value*x+coefficient
    return value

def investigate(text):
    result = {}
    for name, order in [('ABYule',10),('ABButter',2)]:
        packed, interleaved = tables(text,name)
        result[name] = {}
        for rate, vector, scalar in zip(RATES,packed,interleaved):
            b, a = scalar[::2], [1.]+scalar[1::2]
            # Legacy SSE2 tables have a padding entry in each section.
            if order == 10:
                assert vector[:11] == b and vector[12:22] == a[1:]
            else:
                assert vector[:3] == b and vector[4:] == a[1:]
            entry = schur(a)
            entry['denominator_at_one'] = polynomial(a,1.)
            if entry['denominator_at_one'] < 0:
                # A negative P(1) and positive leading coefficient prove at
                # least one real pole > 1. Locate one crossing by bisection.
                lo, hi = 1., 2.
                while polynomial(a,hi) < 0: hi *= 2
                for _ in range(80):
                    mid = (lo+hi)/2
                    if polynomial(a,mid) < 0: lo = mid
                    else: hi = mid
                entry['real_pole_above_one'] = (lo+hi)/2
            result[name][str(rate)] = entry
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ref',default='3fe2589')
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    old = subprocess.check_output(['git','show',f'{args.ref}:gain_analysis.c'],cwd=ROOT).decode('latin1')
    current = (ROOT/'gain_analysis.c').read_bytes().decode('latin1')
    report = {'baseline_ref':args.ref,'method':'Schur reciprocal-polynomial recursion in binary64; P(1) sign and real-pole bisection',
              'baseline':investigate(old),'working_tree':investigate(current)}
    assert all(entry['stable'] for table in report['working_tree'].values() for entry in table.values())
    text = json.dumps(report,indent=2)+'\n'
    if args.output: args.output.write_text(text)
    else: print(text,end='')

if __name__ == '__main__': main()

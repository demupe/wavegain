"""Historical loop-prototype probe; production SIMD checks are in test/filter_regressions.py."""
import argparse
import array
import json
import math
from pathlib import Path
import shlex
import subprocess
import tempfile

from benchmark_conversion import variant_sources
from source_snapshot import revision_sources, write_flat_sources

API_PROBE = r'''
#include <math.h>
#include "gain_analysis.c"
int main(void) {
    double left[512], right[512];
    InitGainAnalysis(88200);
    for (int block = 0; block < 20; block++) {
        for (int j = 0; j < 512; j++)
            left[j] = right[j] = 6553.4 * sin(2 * 3.141592653589793 * 440 * (block*512+j) / 88200);
        AnalyzeSamples(left, right, 512, 2);
    }
    return 0;
}
'''

PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include "gain_analysis.c"
#define COUNT 4096
static double left[COUNT+10],right[COUNT+10];
static double yl[COUNT+10],yr[COUNT+10],bl[COUNT+10],br[COUNT+10];
static uint32_t rng=7654321;
static double value(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (int32_t)rng * (32767. / 2147483648.);
}
int main(void) {
    for (int k=0;k<12;k++) {
        memset(yl,0,sizeof(yl));memset(yr,0,sizeof(yr));
        memset(bl,0,sizeof(bl));memset(br,0,sizeof(br));
        for(int i=0;i<COUNT;i++) {left[i+10]=value();right[i+10]=value();}
#ifdef PAIR
        bench_filter_pair(left+10,right+10,yl+10,yr+10,COUNT,ABYule[k],10,1e-10);
        bench_filter_pair(yl+10,yr+10,bl+10,br+10,COUNT,ABButter[k],2,0.);
#else
        filterYule(left+10,yl+10,COUNT,ABYule[k]);filterYule(right+10,yr+10,COUNT,ABYule[k]);
        filterButter(yl+10,bl+10,COUNT,ABButter[k]);filterButter(yr+10,br+10,COUNT,ABButter[k]);
#endif
        fwrite(yl+10,sizeof(double),COUNT,stdout);fwrite(yr+10,sizeof(double),COUNT,stdout);
        fwrite(bl+10,sizeof(double),COUNT,stdout);fwrite(br+10,sizeof(double),COUNT,stdout);
    }
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--ref", default="74e50db", help="Git source snapshot for the historical prototype")
    args = parser.parse_args()
    original = revision_sources(args.ref)
    report = {"source_ref": args.ref}
    with tempfile.TemporaryDirectory(prefix="wavegain-filter-equivalence-") as temp:
        folder = Path(temp)
        for mode, flags in [("default_contract", []), ("contract_off", ["-ffp-contract=off"])]:
            outputs = []
            for variant in ("baseline", "pair_filter"):
                build = folder / (mode + variant); build.mkdir()
                write_flat_sources(build, variant_sources(variant, original), ref=args.ref)
                (build / "probe.c").write_text(PROBE)
                binary = build / "probe"
                subprocess.run([*shlex.split(args.cc), "-O2", "-DHAVE_CONFIG_H", *flags,
                                *(["-DPAIR"] if variant == "pair_filter" else []),
                                str(build / "probe.c"), "-o", str(binary), "-lm"], check=True, capture_output=True)
                outputs.append(subprocess.check_output([str(binary)]))
            a,b = (array.array("d", output) for output in outputs)
            count = sum(outputs[0][i:i+8] != outputs[1][i:i+8] for i in range(0,len(outputs[0]),8))
            per_rate = {}
            rates = [96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000]
            for index, rate in enumerate(rates):
                start = index * 4096 * 4
                left, right = a[start:start+4096*4], b[start:start+4096*4]
                finite = [(x,y) for x,y in zip(left,right) if math.isfinite(x) and math.isfinite(y)]
                per_rate[str(rate)] = {
                    "baseline_nonfinite": sum(not math.isfinite(x) for x in left),
                    "pair_nonfinite": sum(not math.isfinite(x) for x in right),
                    "baseline_max_abs_finite": max(abs(x) for x in left if math.isfinite(x)),
                    "pair_max_abs_finite": max(abs(y) for y in right if math.isfinite(y)),
                    "max_absolute_difference_finite": max(abs(x-y) for x,y in finite),
                }
            report[mode] = {"per_rate": per_rate, "double_values": len(a), "bitwise_different_values": count,
                            "max_absolute_difference": max(abs(x-y) for x,y in zip(a,b)),
                            "bitwise_equal": outputs[0] == outputs[1]}
        build = folder / "default_contractbaseline"
        probe = build / "api-probe.c"
        probe.write_text(API_PROBE)
        binary = build / "api-probe"
        subprocess.run([*shlex.split(args.cc), "-O2", "-DHAVE_CONFIG_H",
                        "-fsanitize=float-cast-overflow", "-fno-sanitize-recover=float-cast-overflow",
                        str(probe), "-o", str(binary), "-lm"], check=True, capture_output=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        report["88200_api_probe"] = {
            "rate": 88200, "frames": 10240, "tone_hz": 440, "normalized_amplitude": .2,
            "exit_code": result.returncode,
            "diagnostic": result.stderr.replace(str(build) + "/", ""),
        }
    text = json.dumps(report,indent=2)+"\n"
    if args.output: args.output.write_text(text)
    else: print(text,end="")


if __name__ == "__main__": main()

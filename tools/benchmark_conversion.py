"""Benchmark isolated conversion experiments; production sources are not edited.

Builds temporary source copies, generates stereo 96 kHz PCM and compares both
CPU/wall time and complete output bytes. Results describe warm-cache workloads,
not cold storage throughput. Historical variants default to --ref 74e50db;
--variants baseline,working_tree compares that baseline with current sources.
Run with --output /tmp/conversion-performance.json.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import resource
import shlex
import statistics
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def replace(text, old, new, count=1):
    if text.count(old) != count:
        raise RuntimeError("Source changed; review experiment anchor: " + old[:100])
    return text.replace(old, new)


def variant_sources(kind, source):
    source = dict(source)
    if kind in ("stats_compare", "combined"):
        s = source["wavegain.c"].decode()
        s = replace(s, "info->minimum[ch] = fmin(info->minimum[ch], value);",
                    "if (value < info->minimum[ch]) info->minimum[ch] = value;")
        s = replace(s, "info->maximum[ch] = fmax(info->maximum[ch], value);",
                    "if (value > info->maximum[ch]) info->maximum[ch] = value;")
        source["wavegain.c"] = s.encode()
    if kind in ("reuse_buffers", "combined"):
        s = source["wavegain.c"].decode()
        s = replace(s, "\t\twhile (1) {", "\t\tsample_buffer = malloc(sizeof(double) * wg_opts->channels * BUFFER_LEN);\n\t\twhile (1) {")
        s = replace(s, "\t\t\t\tsample_buffer = malloc(sizeof(double) * wg_opts->channels * bout);", "")
        s = replace(s, "\t\t\t\tfree(sample_buffer);", "")
        s = replace(s, "\t\tfor (i = 0; i < wg_opts->channels; i++)\n\t\t\tif (pcm[i]) free(pcm[i]);",
                    "\t\tfree(sample_buffer);\n\t\tfor (i = 0; i < wg_opts->channels; i++)\n\t\t\tif (pcm[i]) free(pcm[i]);")
        source["wavegain.c"] = s.encode()
        s = source["audio.h"].decode()
        s = replace(s, "\tUint64_t samples;", "\tUint64_t samples;\n\tvoid *scratch;\n\tsize_t scratch_capacity;")
        source["audio.h"] = s.encode()
        s = source["audio.c"].decode()
        first = s.index("int write_audio_8bit(")
        helpers = '''static void *bench_scratch(audio_file *out, size_t bytes)
{
    if (out->scratch_capacity < bytes) {
        void *data = realloc(out->scratch, bytes);
        if (!data) return NULL;
        out->scratch = data;
        out->scratch_capacity = bytes;
    }
    return out->scratch;
}

'''
        s = s[:first] + helpers + s[first:]
        tail = s[first:]
        tail = replace(tail, "malloc(samples*aufile->bits_per_sample*sizeof(char)/8)",
                       "bench_scratch(aufile, samples*aufile->bits_per_sample/8)", 5)
        tail = replace(tail, "\tif (data)\n\t\tfree(data);", "", 5)
        s = s[:first] + tail
        s = replace(s, "\tif (fclose(aufile->sndfile)) ok = 0;", "\tfree(aufile->scratch);\n\tif (fclose(aufile->sndfile)) ok = 0;")
        source["audio.c"] = s.encode()
    if kind in ("native_float", "combined"):
        s = source["audio.c"].decode()
        a = s.index("int write_audio_float(")
        b = s.index("\nvoid* output_to_PCM", a)
        s = s[:a] + '''/* Investigation prototype: IEEE-754 binary32, little-endian host only.
 * Canonicalize signed zero as the existing writer does. Subnormal semantics
 * must be resolved before this can become a production fast path. */
int write_audio_float(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
    float *values = sample_buffer;
    unsigned int i;
    for (i = 0; i < samples; i++) if (values[i] == 0.f) values[i] = 0.f;
    aufile->samples += samples;
    return fwrite(values, samples * sizeof(float), 1, aufile->sndfile);
}
''' + s[b:]
        source["audio.c"] = s.encode()
    if kind in ("pair_filter", "combined"):
        text = source["gain_analysis.c"].decode("latin1")
        anchor = text.index("// returns a INIT_GAIN_ANALYSIS_OK", text.index("filterButter("))
        helper = """/* Investigation prototype: lanes contain independent left/right histories. */
typedef double bench_pair __attribute__((vector_size(16)));
static void bench_filter_pair(const Float_t *left, const Float_t *right,
    Float_t *outleft, Float_t *outright, size_t count, const Float_t *kernel,
    int order, double bias)
{
    while (count--) {
        bench_pair input = {left[0], right[0]};
        bench_pair result = input * kernel[0];
        if (bias) result = bias + result;
        for (int j = 1; j <= order; j++) {
            bench_pair history = {outleft[-j], outright[-j]};
            bench_pair prior = {left[-j], right[-j]};
            result = result - history * kernel[2*j-1];
            result = result + prior * kernel[2*j];
        }
        *outleft++ = result[0]; *outright++ = result[1];
        left++; right++;
    }
}

"""
        text = text[:anchor] + helper + text[anchor:]
        old = """        YULE_FILTER ( curleft , lstep + totsamp, cursamples, ABYule[freqindex]);
        YULE_FILTER ( curright, rstep + totsamp, cursamples, ABYule[freqindex]);

        BUTTER_FILTER ( lstep + totsamp, lout + totsamp, cursamples, ABButter[freqindex]);
        BUTTER_FILTER ( rstep + totsamp, rout + totsamp, cursamples, ABButter[freqindex]);"""
        new = """        if (num_channels == 2) {
            bench_filter_pair(curleft, curright, lstep+totsamp, rstep+totsamp,
                              cursamples, ABYule[freqindex], YULE_ORDER, 1e-10);
            bench_filter_pair(lstep+totsamp, rstep+totsamp, lout+totsamp, rout+totsamp,
                              cursamples, ABButter[freqindex], BUTTER_ORDER, 0.);
        } else {
""" + old + """
        }"""
        text = replace(text, old, new)
        source["gain_analysis.c"] = text.encode("latin1")
    return source


def instrument(source):
    source = dict(source)
    s = source["wavegain.c"].decode()
    a = s.index("/* Get the gain and peak value")
    helper = '''static double bench_times[7];
static void bench_report(void) {
    fprintf(stderr, "\\nBENCH_STAGE [%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f]\\n",
      bench_times[0],bench_times[1],bench_times[2],bench_times[3],
      bench_times[4],bench_times[5],bench_times[6]);
}
__attribute__((constructor)) static void bench_init(void) { atexit(bench_report); }
#define BENCH_TIME(index, expression) ({ clock_t start_ = clock(); \\
    __typeof__(expression) value_ = (expression); \\
    bench_times[index] += (double)(clock() - start_) / CLOCKS_PER_SEC; value_; })

'''
    s = s[:a] + helper + s[a:]
    s = replace(s, "count = opt.read_samples(opt.readdata, buffer, BUFFER_LEN, sampled, position);",
                "count = BENCH_TIME(0, opt.read_samples(opt.readdata, buffer, BUFFER_LEN, sampled, position));")
    s = replace(s, "        frames += count;", "        clock_t stats_start = clock();\n        frames += count;")
    s = replace(s, "        if (AnalyzeSamples(buffer[0], buffer[1], count, opt.channels) != GAIN_ANALYSIS_OK) goto done;",
                "        bench_times[1] += (double)(clock() - stats_start) / CLOCKS_PER_SEC;\n"
                "        if (BENCH_TIME(2, AnalyzeSamples(buffer[0], buffer[1], count, opt.channels)) != GAIN_ANALYSIS_OK) goto done;")
    s = replace(s, "readcount = wg_opts->read_samples(wg_opts->readdata, pcm, BUFFER_LEN, 0, 0);",
                "readcount = BENCH_TIME(3, wg_opts->read_samples(wg_opts->readdata, pcm, BUFFER_LEN, 0, 0));")
    s = replace(s, "\t\t\t\tfor(k = 0; k < wg_opts->channels; k++) {",
                "\t\t\t\tclock_t transform_start = clock();\n\t\t\t\tfor(k = 0; k < wg_opts->channels; k++) {")
    s = replace(s, "\t\t\t\tsample_buffer = output_to_PCM(pcm, sample_buffer, wg_opts->channels,\n\t\t\t\t\t\tbout, wg_opts->format);",
                "\t\t\t\tbench_times[4] += (double)(clock() - transform_start) / CLOCKS_PER_SEC;\n"
                "\t\t\t\tsample_buffer = BENCH_TIME(5, output_to_PCM(pcm, sample_buffer, wg_opts->channels, bout, wg_opts->format));")
    s = replace(s, "write_audio_file(aufile, sample_buffer, bout * wg_opts->channels)",
                "BENCH_TIME(6, write_audio_file(aufile, sample_buffer, bout * wg_opts->channels))")
    source["wavegain.c"] = s.encode()
    return source


def fixture(path, seconds, bits):
    rate = 96000
    frame_count = round(seconds * rate)
    width = bits // 8
    data_bytes = frame_count * 2 * width
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + data_bytes, b"WAVE", b"fmt ", 16,
                         1, 2, rate, rate * 2 * width, 2 * width, bits, b"data", data_bytes)
    block = bytearray()
    for i in range(rate):
        for frequency, dc in ((440, .01), (659, -.02)):
            x = dc + .28 * math.sin(2 * math.pi * frequency * i / rate) + .05 * math.sin(2 * math.pi * 47 * i / rate)
            block.extend(round(x * ((1 << (bits - 1)) - 1)).to_bytes(width, "little", signed=True))
    with path.open("wb") as out:
        out.write(header)
        remaining = data_bytes
        while remaining:
            chunk = block[:min(remaining, len(block))]
            out.write(chunk)
            remaining -= len(chunk)


def execute(binary, args, output):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    with output.open("wb") as target:
        result = subprocess.run([str(binary), *map(str, args)], stdout=target, stderr=subprocess.PIPE)
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    import re
    matches = re.findall(rb"BENCH_STAGE (\[[^\n]+\])", result.stderr)
    return {"wall_seconds": wall, "user_seconds": after.ru_utime - before.ru_utime,
            "system_seconds": after.ru_stime - before.ru_stime,
            "stages": json.loads(matches[-1]) if matches else None,
            "stderr": result.stderr.decode(errors="replace")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=120)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--keep-builds", type=Path)
    parser.add_argument("--fp-contract-off", action="store_true")
    parser.add_argument("--ref", default="74e50db", help="Git baseline for source experiments")
    parser.add_argument("--variants", help="Comma-separated variants; baseline must come first")
    args = parser.parse_args()
    if args.seconds <= 0 or args.repeats < 2: parser.error("positive duration and at least two repeats required")
    cc = shlex.split(args.cc)
    report = {"source_commit": subprocess.check_output(["git", "rev-parse", args.ref], cwd=ROOT, text=True).strip(),
              "platform": platform.system(), "architecture": platform.machine(),
              "compiler": subprocess.check_output([*cc, "--version"], text=True).splitlines()[0],
              "fp_contract_off": args.fp_contract_off, "audio_seconds": args.seconds, "sample_rate": 96000, "channels": 2, "repeats": args.repeats,
              "method": "One warmup per pair; randomized interleaved run order, warm filesystem cache, output file writes included.",
              "cases": {}, "profile": {}}
    import random
    variants = ["baseline", "stats_compare", "reuse_buffers", "native_float", "pair_filter", "combined", "O3", "O3_LTO"]
    if args.variants:
        variants = args.variants.split(",")
        if not variants or variants[0] != "baseline" or any(name not in
            ["baseline", "stats_compare", "reuse_buffers", "native_float", "pair_filter", "combined", "O3", "O3_LTO", "working_tree"] for name in variants):
            parser.error("baseline must come first; unknown variant name")
    cases = {"calculate24": (24, ["-c"]), "apply16": (16, ["-y", "-o", "--force"]),
             "apply24": (24, ["-y", "-o", "--force"]), "float24": (24, ["-y", "-o", "--force", "-b", "5"]),
             "dither24": (24, ["-y", "-o", "--force", "-d", "2"]),
             "limiter24": (24, ["-y", "-o", "--force", "-n", "-g", "12", "-t"])}
    with tempfile.TemporaryDirectory(prefix="wavegain-conversion-bench-") as temp:
        folder = Path(temp)
        names = subprocess.check_output(["git", "ls-tree", "--name-only", args.ref], cwd=ROOT, text=True).splitlines()
        original = {name: subprocess.check_output(["git", "show", f"{args.ref}:{name}"], cwd=ROOT)
                    for name in names if name.endswith((".c", ".h"))}
        current = {p.name: p.read_bytes() for p in [*ROOT.glob("*.c"), *ROOT.glob("*.h")]}
        if "working_tree" in variants:
            report["working_tree_source_sha256"] = hashlib.sha256(b"".join(current[name] for name in sorted(current))).hexdigest()
        binaries = {}
        for name in [*variants, "profile"]:
            build = folder / name; build.mkdir()
            source = current if name == "working_tree" else (instrument(original) if name == "profile" else variant_sources(name, original))
            for filename, data in source.items(): (build / filename).write_bytes(data)
            flags = ["-O3"] if name.startswith("O3") else ["-O2"]
            if name == "O3_LTO": flags.append("-flto")
            if args.fp_contract_off: flags.append("-ffp-contract=off")
            binary = build / "wavegain"
            result = subprocess.run([*cc, *flags, "-D_FILE_OFFSET_BITS=64", "-DHAVE_CONFIG_H", "-D_FORTIFY_SOURCE=2",
                                     "-o", str(binary), *map(str, sorted(build.glob("*.c"))), "-lm"], capture_output=True)
            if result.returncode: raise RuntimeError(result.stderr.decode(errors="replace"))
            binaries[name] = binary
        inputs = {}
        for bits in (16, 24):
            path = folder / f"input{bits}.wav"; fixture(path, args.seconds, bits); inputs[bits] = path
        output = folder / "output.bin"
        hashes = {}
        for case, (bits, flags) in cases.items():
            report["cases"][case] = {}
            for name in variants:
                execute(binaries[name], [*flags, inputs[bits]], output)
                hashes[case, name] = hashlib.sha256(output.read_bytes()).hexdigest()
                report["cases"][case][name] = {"runs": [], "output_matches_baseline": hashes[case, name] == hashes[case, "baseline"]}
        schedule = [(case, name) for _ in range(args.repeats) for case in cases for name in variants]
        random.Random(741).shuffle(schedule)
        for index, (case, name) in enumerate(schedule):
            bits, flags = cases[case]
            result = execute(binaries[name], [*flags, inputs[bits]], output)
            result.pop("stderr"); result.pop("stages")
            report["cases"][case][name]["runs"].append(result)
            if (index + 1) % 21 == 0: print(f"benchmarked {index+1}/{len(schedule)} runs", flush=True)
        for case, results in report["cases"].items():
            for name, entry in results.items():
                for metric in ("wall_seconds", "user_seconds", "system_seconds"):
                    entry["median_" + metric] = statistics.median(r[metric] for r in entry["runs"])
                entry["wall_range_seconds"] = [min(r["wall_seconds"] for r in entry["runs"]), max(r["wall_seconds"] for r in entry["runs"])]
                entry["speedup"] = results["baseline"]["median_wall_seconds"] / entry["median_wall_seconds"]
        for case, (bits, flags) in cases.items():
            measured = [execute(binaries["profile"], [*flags, inputs[bits]], output) for _ in range(3)]
            stages = [statistics.median(r["stages"][i] for r in measured) for i in range(7)]
            report["profile"][case] = dict(zip(["analysis_read_decode", "analysis_statistics", "replaygain_filter", "apply_read_decode",
                                                  "gain_dither_limiter", "output_to_PCM", "serialize_write"], stages))
        # Edge-case byte semantics are explicitly inspected, separate from throughput.
        edge = folder / "float_edges.c"
        edge.write_text('''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"
int main(void) {
 uint32_t bits[] = {0x00000000,0x80000000,0x00000001,0x007fffff,0x00800000,0x3f000000,0xbf000000};
 float values[7]; memcpy(values,bits,sizeof(bits)); audio_file out={0}; out.sndfile=stdout; out.bits_per_sample=32;
 return write_audio_float(&out,values,7) != 1;
}
''')
        report["float_edge_bytes"] = {}
        for name in ("baseline", "native_float"):
            build = folder / name; binary = folder / ("edge-" + name)
            if not build.exists():
                build.mkdir()
                for filename, content in variant_sources(name, original).items():
                    (build / filename).write_bytes(content)
            subprocess.run([*cc, "-O2", "-DHAVE_CONFIG_H", "-D_FILE_OFFSET_BITS=64", "-I", str(build), "-o", str(binary),
                            str(edge), str(build / "audio.c"), str(build / "w64.c"), "-lm"], check=True, capture_output=True)
            report["float_edge_bytes"][name] = subprocess.check_output([str(binary)]).hex()
        if args.keep_builds:
            import shutil
            args.keep_builds.mkdir(parents=True, exist_ok=True)
            for name in variants:
                shutil.copytree(folder / name, args.keep_builds / name, dirs_exist_ok=True)
    serialized = json.dumps(report, indent=2) + "\n"
    if args.output: args.output.write_text(serialized)
    else: print(serialized)


if __name__ == "__main__": main()

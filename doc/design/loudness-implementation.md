# Loudness and output feature implementation

Implementation record for features introduced in v1.5.0. Current usage is in
[README](../../README); remaining work is in [TODO](../../TODO). Measurement
evidence is kept in [performance/](../performance/).

This branch adds separate output paths, true-peak measurement and ceilings,
BS.1770 integrated loudness, 176.4/192 kHz input, and an optional rate-aware
dither family. Existing ReplayGain rates and the legacy dither family retain
their original analysis and quantization paths unless a new option is selected.

## Measurement engine and R128Net mechanisms

The implementation bundles libebur128 v1.2.6 at commit
`67b33abe1558160ed76ada1322329b0e9e058b02` with its MIT license and BSD queue
header. `loudness.c` is the adapter; no system package or .NET runtime is needed.

The optimized double-input kernels in `loudness_fast.h` use mechanisms from
[R128Net 1.1.2](https://github.com/routersys/R128Net/tree/9cc4d4efa05296d3c03bec7ff1e1b218936664e9),
particularly `Interpolator.cs` and `KWeightingFilter.cs`. The R128Net MIT notice
is retained in `vendor/libebur128/R128Net-LICENSE.txt`.

| Mechanism | C implementation |
| --- | --- |
| Fuse interpolation with peak reduction | Do not write and rescan the full oversampled output buffer |
| Contiguous, widened history | Prepare small double-precision blocks from the reference float history; avoid a ring-index branch per tap |
| Independent frames in SIMD lanes | Compute two consecutive interpolation outputs per 128-bit vector; use two independent vectors on AArch64 for dense 4x phases |
| Dense interpolation phases | Validate the 4x coefficient layout and accumulate its three 12-tap phases together, retaining each phase's tap order |
| Conservative peak screening | Skip a block only when a double-precision triangle-inequality bound, with rounding allowance, cannot raise the current call's peak |
| Independent channels in SIMD lanes | Apply K-weighting to stereo channels together, retaining each channel's expression order |
| Narrow the maximum once | Use monotonic float conversion to preserve the reference peak without narrowing every interpolated output |
| Reuse measurement state | Accumulate the entire track; use one state for LUFS and peak together |

This is an adaptation of the relevant kernels, not a complete translation of
the C# library. R128Net's float partial-filter screening and .NET-specific
allocation layout are not implemented. The native screen instead uses the
maximum absolute input (including retained history) times the largest phase
coefficient L1 norm, with a conservative rounding allowance; uncertain blocks
still use the exact double calculation. Screening uses the current call's peak
to preserve `ebur128_prev_true_peak` semantics. FMA contraction is disabled for
comparison with the reference expressions. Denormal handling follows the original native C
implementation; cross-architecture bit equality with .NET is not claimed.

Define `WG_EBUR_REFERENCE` to build the original double-input kernels instead.
The other sample-format entry points remain the upstream implementations;
WaveGain feeds normalized doubles to the adapter.

### Additional native improvements

Stereo gating-block energy now reads the interleaved ring once and accumulates
both channels in SIMD lanes. Each channel retains the original addition order,
including the reference's prefix-then-tail order at ring wrap. Channel weights
and gating comparisons are unchanged. Rolling sums or parallel partial sums
were avoided because their rounding differences can change gate decisions.
Mono and platforms without the paired SIMD backend keep the original loop.

The WaveGain adapter also bypasses K-weighting and loudness-ring writes when
only true peak is requested. It still checks finite input, tracks sample peaks,
uses the same interpolation kernel and drains its tail on completion. This
also removes unnecessary filtering from final output-ceiling verification.
It changes only the private adapter path, not the vendored public add-frames
API. Define `WG_EBUR_EXTRA_REFERENCE` to disable these two additions for
incremental comparisons; later true-peak refinements remain active in that
variant. `WG_EBUR_REFERENCE` still selects the original native kernels.

The interpolation ring now saves only samples that survive the block's final
wrap, advancing an index instead of dividing at every sample. The adapter
specializes mono/stereo packing while retaining finite-input checks. LUFS
analysis avoids writing the unused ReplayGain-scaled samples back to its
input buffers; extrema and sequential DC statistics are unchanged. Output
true-peak verification and its safety reserve remain active.

### Correctness and performance evidence

`make check-loudness` compares original and optimized filter state, audio ring
contents, individual block energies, integrated loudness, sample/true peaks
and histogram counts across
14 rates, mono/stereo, list and histogram integration, irregular block sizes,
silence, subnormals and a final-frame impulse. The peak-only adapter is also
compared for empty, one-frame, short and longer inputs, including repeated
finish calls and interpolation-tail draining. Results match byte-for-byte on
macOS arm64 and on an x86_64 build executed under Rosetta.

Reproduce the CPU-only benchmark:

```sh
python3 test/loudness_kernels.py --benchmark --output /tmp/loudness-performance.json
```

[loudness-kernel-performance.json](../performance/loudness-kernel-performance.json) records five alternating runs on macOS arm64,
Apple Clang 21, `-O2`. Each workload is 60 seconds of synthetic stereo samples,
with state setup and PCM I/O outside the timed interval. Peak-only adapter
timings include planar-to-interleaved packing and final tail draining.

| Workload | Reference (ms) | Previous R128Net-inspired (ms) | Current (ms) | Current speedup vs previous | Current speedup vs reference |
| --- | ---: | ---: | ---: | ---: | ---: |
| Integrated, 48 kHz | 64.880 | 36.702 | 26.108 | 1.41x | 2.49x |
| Integrated + true peak, 48 kHz | 226.950 | 99.769 | 88.950 | 1.12x | 2.55x |
| True peak only, 48 kHz | 209.779 | 79.754 | 76.563 | 1.04x | 2.74x |
| True peak only, 192 kHz | 221.099 | 90.275 | 29.958 | 3.01x | 7.38x |

At 192 kHz the reference requires no interpolation, so eliminating unnecessary
K-weighting has a larger relative effect. These are measurement-engine CPU
timings, not end-to-end file conversion or comparisons to the .NET implementation.
The JSON records raw runs, medians and hashes for all three implementations;
the benchmark harness keeps both earlier paths selectable for reproduction.

## LUFS and true peak semantics

`--lufs` selects integrated loudness, with a default target of -23 LUFS.
`--target-lufs X` selects the same mode and changes the target. Integration
uses the exact block-energy list rather than the optional 0.1-LU histogram;
history memory therefore grows with duration. Large audio/interpolation
buffers are released when a track is finished; album aggregation retains only
its gating history and small state. An album applies a single gain obtained
from pooled gated block energies, not an average of track LUFS values.

DC correction follows the existing track/album selection. With DC correction,
a separate measurement read is necessary after the means are known. With
`-p`, measurement is fused into the first analysis read. Finalized-size WAV or
Wave64 input can also be streamed through stdin with `-p` for calculation.
Unknown-size stdin remains strict. Applying gain still requires file input.

LUFS requires at least 400 ms. Silence and material below the absolute gate
have no finite integrated target gain; only manual gain is selected in that
case. A successful measurement does not implicitly introduce compression or
change sample rate. Peak prevention can leave output below the requested
LUFS target, and the gain reduction is reported.

`--true-peak` enables DC-corrected measurement and a -1 dBTP ceiling by default;
`--peak-ceiling X` changes the ceiling and also enables measurement. The
explicit ceiling remains active with `--noclip`. Gain selection reserves
0.1 dB plus four integer output LSBs (or a small float margin). Impossibly low
ceilings for the chosen bit depth are rejected. Quantized/dithered/limited
output is measured again before publication, and a ceiling violation prevents
file replacement. On stdout, already-emitted audio cannot be retracted and a
failed final check returns nonzero.

The interpolation tail is drained without adding silent blocks to LUFS
integration. FFmpeg comparison tests pad the signal for peak-tail measurement;
the FIR designs and oversampling rates differ, particularly around transients
at 176.4 kHz. The independent check allows 0.3 dB peak difference and 0.11 LU
integrated difference, while the original/optimized kernel comparison is exact.

## High-rate ReplayGain analysis

For 176.4 and 192 kHz only, a streaming 127-tap Blackman-windowed FIR decimates
the analysis signal to 88.2 and 96 kHz respectively. Its cutoff is 0.225 cycles
per input sample. The 63-sample group delay is compensated and the tail is
flushed, retaining exactly `ceil(input_frames / 2)` analysis frames. Statistics
and peak protection continue to use the original PCM, and output rate/frame
count are unchanged. Existing ReplayGain rates bypass this decimator.

LUFS and true peak instead operate directly at the original input rate.

## Rate-aware dither

The default remains `--dither-profile legacy`. `--dither-profile rate` requires
`-d 1..4` and integer output. It uses TPDF noise and first-order error feedback,
with transfer function `1 - a*z^-1` and `a = exp(-2*pi*knee/sample_rate)`.
Light, Medium and Heavy select 10, 6 and 3 kHz knees; unshaped TPDF uses `a=0`.
This is a distinct family, not a retuning of the legacy F44 filters. Feedback
is bounded and is reset on saturation to avoid windup.

The signal tests check near-zero mean, noise energy, unshaped correlation,
rate-dependent high-frequency emphasis and clipping bounds. These establish
the implementation's properties, not a claim of perceptual optimality or a
substitute for listening tests.

## Separate output publication

`-y --output FILE` requires one explicit file. `--output-dir DIR` supports
multiple files and preserves relative recursive subdirectories. Directories
are created as needed; an output root inside an input directory is rejected.
The original remains unchanged and existing destinations, including symlinks,
are never replaced. Duplicate output basenames in a batch are rejected.

Conversion uses an exclusive temporary in the destination directory. On POSIX,
separate output inherits the input's ordinary permission bits (0777), without
copying owner/group or special mode bits. This replaces mkstemp's initial 0600
mode before publication; filesystems without POSIX modes may emulate permissions.

macOS first tries `renamex_np(RENAME_EXCL)` and Linux tries
`renameat2(RENAME_NOREPLACE)` through its syscall for compatibility with older
libc versions. If the platform/filesystem lacks the exclusive operation,
link/unlink provides atomic no-clobber publication where hard links work.
Neither path uses a check-then-rename sequence that could overwrite a racer.

When hard links are also unavailable (including the tested macOS exFAT image),
the completed temporary is copied into a destination opened with
`O_CREAT | O_EXCL`. Existing files and symlinks remain protected. This final
fallback is not atomically visible: readers can see a partial copy. It requires
space for both files during copying. On failure it reports and retains the
incomplete destination and completed temporary, rather than risk unlinking
another process's replacement of the destination. Successful copying restores
ordinary permissions, checks close errors, then removes the temporary.

Windows uses MoveFileExA without replacement flags. Native Windows runtime/path
validation remains open, as for the existing Windows code.

The publication regression test disables hard links and injects unavailable
native operations, short/interrupted writes and ENOSPC. It checks successful
fallbacks, permissions, collisions with files/directories/dangling symlinks,
and recovery files after failure. It passed on macOS arm64 and Linux x86_64
in a local emulated container. Both separate-output options also passed on
an actual mounted exFAT image copy, where a hard-link probe returned ENOTSUP.

## Validation completed

`make check-sanitize` passed the full regression gate with AddressSanitizer
and UndefinedBehaviorSanitizer enabled. `make verified-release` and
`make check-interop` also passed. A separate comparison against v1.4.5 matched
120 ordinary WAV conversions byte-for-byte across four existing sample rates,
mono/stereo, three integer output widths and all five legacy dither settings.
`make debug` introduced no new warnings; the existing non-UTF-8 copyright
comment warning in `gain_analysis.c` remains unchanged.

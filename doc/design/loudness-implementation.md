# Loudness implementation

See [README](../../README) for options and behavior, and
[test/README.txt](../../test/README.txt) for verification commands.

## Engine and licenses

`src/loudness.c` adapts bundled libebur128 v1.2.6
(`67b33abe1558160ed76ada1322329b0e9e058b02`). Its MIT license and BSD queue
notice are retained under `vendor/libebur128/`.

`src/loudness_fast.h` adapts mechanisms from MIT-licensed
[R128Net 1.1.2](https://github.com/routersys/R128Net/tree/9cc4d4efa05296d3c03bec7ff1e1b218936664e9).
Its notice is `vendor/libebur128/R128Net-LICENSE.txt`. No system libebur128
or .NET runtime is required.

## Precision contracts

- ReplayGain and LUFS remain separate modes. LUFS/true peak use source-rate
  audio; high-rate ReplayGain decimates analysis only, preserving output frames.
- Integrated LUFS retains exact block energies. Album integration pools
  gating history rather than averaging track LUFS. Memory grows with duration.
- SIMD retains each channel/phase's operation order. FP contraction and
  reassociation are disabled; do not build with fast-math.
- The CLI configures a 385-tap, 16x true-peak interpolator by default.
  `--true-peak-mode fast` selects 49 taps/4x at every rate, including 192 kHz.
  The selected mode covers input and output meters and never skips frames.
  LUFS/ReplayGain calculations do not change. Fast can under-read by more
  than the 0.1 dB gain reserve; it is not the sampled ReplayGain `--fast` mode.
  Low-level bundled-library reference tests retain their original filters.
  AVX2 processes four independent frames for generic phase layouts too.
  Known-amplitude/phase and independent sinc reconstruction checks complement
  kernel equality. A finite FIR plus 0.1 dB reserve is not a universal bound
  on continuous-time peaks or later sample-rate conversion; higher precision
  increases true-peak CPU cost and formal conformance validation remains open.
- Dense 4x interpolation validates its coefficient layout. AArch64 and
  x86_64 schedule four independent frames. On supported x86_64 compilers,
  runtime CPU detection selects an AVX2 function for this block only; other
  CPUs use 128-bit SIMD, and other layouts retain their general kernel.
- Peak screening uses a conservative coefficient-L1/input-magnitude bound
  with rounding allowance. Uncertain blocks use exact interpolation;
  per-call peak semantics and interpolation history remain intact.
- Gain selection reserves peak headroom. Actual quantized/dithered output
  is checked before publication; stdout cannot retract bytes already emitted.
- Separate outputs use no-clobber publication. The final exclusive-copy
  fallback can be partially visible; failures retain recovery files.

`WG_EBUR_REFERENCE` selects original loudness kernels. `WG_EBUR_EXTRA_REFERENCE`
disables stereo-energy SIMD and the peak-only adapter bypass.
`WG_TP_FULL_INTERPOLATION` disables peak screening; `WG_TP_PAIR_ONLY` selects
two-frame dense processing. These are compile-time verification controls.
`WG_TP_AVX2=0` disables AVX2 dispatch without changing the baseline target ISA.
ReplayGain's `WAVEGAIN_STEREO_HISTORY=0` selects its two-pass SIMD fallback.

Use `make verified-release`, `make check`, `make check-sanitize` and
`make check-interop` as appropriate. Reference equality does not establish
formal ITU/EBU conformance; remaining validation is listed in [TODO](../../TODO).
Detailed experiments and measurement data are local-only under the gitignored
`local-research/` directory.

WaveGain documentation
======================

Start here
----------
../README                 Current usage, build/test commands and behavior changes.
../README.ja.md           Japanese usage and development guide.
../TODO                   Current fix status and remaining work.
../test/README.txt        Test inventory, subsets, platforms and sanitizer usage.
manual/index.html         Generated CLI reference.
help.json                 Canonical option/help definitions; edit this source.

The documentation is grouped by purpose:
  design/       Implementation and architecture records.
  performance/  Optimization investigations and their measurement evidence.
  reviews/      Safety reviews, fixes and before/after investigation evidence.
  manual/       Generated CLI reference and explicitly historical manual notes.

Current usage and open work belong in the root README files and TODO.
Reports describe their recorded commit, compiler and platform; old findings
or validation gaps may have been resolved later. Consult the associated fix
report and TODO before treating an old observation as current behavior.
JSON measurements remain beside their reports. Recorded timings, hashes and
provenance are historical evidence, not guarantees on other platforms.

Implementation
--------------
design/loudness-implementation.md
  LUFS, true peak, separate outputs, high-rate input and dither implementation;
  bundled libebur128/R128Net mechanisms, validation and licensing references.
  Associated measurements: performance/loudness-kernel-performance.json.

Performance investigations
-------------------------
Read these for the mechanism, exact-output checks, rejected alternatives and
measurement limits. Multiple reports can cover successive incremental changes;
their speed ratios must not be combined into an unsupported overall claim.

performance/lufs-csharp-refinement-2026-10-18.txt
performance/lufs-csharp-refinement-2026-10-18.json
  Repeated C#/native gap analysis: dense interpolation, independent frame
  scheduling, conservative peak screening and ring/adapter work reduction;
  exact-output checks and same-real-program analysis/application timings.

performance/macos-arm-optimization-audit-2026-10-18.txt
performance/macos-arm-optimization-audit-2026-10-18.json
  Apple Silicon compiler/assembly audit: Windows ARM64-tested audio
  optimizations are already enabled; isolated stereo-history measurements
  and standard verified-release exact-output checks.

performance/macos-validation-2026-10-18.txt
performance/macos-validation-2026-10-18.json
  Native Apple M1 Pro validation against v1.5.0: core/sanitizer/FFmpeg gates,
  complete-file equivalence, safety comparisons and matched-flag benchmarks.

performance/stereo-optimization.txt
performance/stereo-performance-windows.json
  ARM64 stereo filter histories/stage fusion and local peak/DC scans;
  x64 retains its former paired-filter path.

performance/mono-optimization.txt
performance/mono-performance-windows.json
  Identical mono-channel filter reuse and mixed-channel state validation.

performance/pcm-optimization.txt
performance/pcm-performance-windows.json
performance/gain-performance-windows.json
  Integer PCM decoding, high-rate FIR histories and integer gain specialization.

performance/filter-simd-investigation.txt
performance/filter-performance.json
performance/filter-stability.json
performance/filter-equivalence-arm64.json
performance/filter-equivalence-x86_64.json
performance/filter-equivalence-gcc.json
performance/filter-equivalence-sanitize.json
performance/filter-equivalence-x86-sanitize.json
performance/filter-88200-interop.json
performance/filter-88200-legacy-interop.json
  Original paired ReplayGain SIMD, arithmetic equivalence and the 88.2 kHz
  coefficient correction. These are dated baseline/platform records.

performance/conversion-safe-implementation.txt
performance/conversion-safe-performance.json
  Reusable conversion/packing buffers, bounded writes and failure handling.

performance/conversion-performance.txt
performance/conversion-performance.json
performance/conversion-contract-performance.json
performance/conversion-filter-equivalence.json
  Early conversion experiments, including rejected prototypes and FP behavior.

performance/loudness-kernel-performance.json
  Measurement-engine kernel evidence; interpretation is in the design record.

Safety reviews and fix records
-----------------------------
reviews/test-suite-review-2026-10-18.txt
reviews/test-suite-review-2026-10-18.json
  Shared test runner, strict failure checks, newly covered boundaries and
  remaining platform/conformance gaps.

reviews/review-followup-fixes-2026-10-18.txt
reviews/review-followup-fixes-2026-10-18.json
  Resolution and validation of all six follow-up findings: undo scales,
  command bounds, duplicate inputs, partial frames, DC display and valid bits.

reviews/review-followup-2026-10-18.txt
reviews/review-followup-2026-10-18.json
  Original observations at 2793ef4, retained for comparison. The six findings
  are addressed by the fix record above; this is not an open-bug list.

reviews/windows-directory-quality.txt
  Junction/symlink traversal, physical output containment and Windows tests.

reviews/code-review-2026-10-16.md
  Original R1-R11 container/I/O/memory review and its implementation follow-up.
  Source line numbers refer to that review's recorded baseline.

reviews/todo-fixes.txt
reviews/todo-fixed.json
reviews/todo-investigation.txt
reviews/todo-investigation.json
  Initial v1.3.2 fork investigation and early fixes: native-width PCM,
  Wave64, DC/clipping/progress and help generation. Preserve the distinction
  between baseline observations and corrected results.

Manual maintenance and historical assets
---------------------------------------
manual/legacy-notes.txt    Historical prose, not the current option reference.
manual/image001.png       Retained historical manual image.

Run commands from the repository root:
  make docs              Regenerate help_generated.h, the README help block,
                         debian/wavegain.1 and doc/manual/index.html.
  make check-docs        Verify generated files and documented options.
Without make:
  python tools/generate_help.py --check

Keep help.json and manual/index.html at their established paths: generation,
tests and packaging refer to them. When moving other reports, update root
README/TODO references, source links in Markdown and this index. Keep paired
JSON evidence byte-for-byte; do not rewrite recorded metrics or hashes.

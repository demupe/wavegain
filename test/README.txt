WaveGain test guide
===================

One inventory, two build entry points
-------------------------------------
test/run_suite.py is the canonical test inventory for both platforms.
  make check
    Builds the main binary when needed, then runs the core suite with CC,
    CPPFLAGS, CFLAGS, DEFS and linker flags. C checks use -UNDEBUG.
  python tools/check_windows.py --cc aarch64-w64-mingw32-clang --optimized
    Always builds the Windows binary, then delegates to the same suite.
    Omit --optimized for O2; use x86_64-w64-mingw32-clang for x64 emulation
    on an ARM64 host. Compiler/runtime availability is required.

Tests use only Python's standard library and C. Test C executables are built
in an isolated temporary directory, not into test/. Tests require Python
assertions: -O/PYTHONOPTIMIZE is rejected rather than silently disabling checks.
When changing compiler/flags with make, rebuild the main binary as usual;
make does not track flag changes as file dependencies.

List or select tests
--------------------
  python test/run_suite.py --list
  python test/run_suite.py --only docs --only support_contracts
  python test/run_suite.py --only followup_regressions --binary ./wavegain
  python test/run_suite.py --only portability --cc cc --cflags="-O2 -D_FILE_OFFSET_BITS=64 -DHAVE_CONFIG_H"

--only can be repeated. For CLI scripts, --binary always selects the exact
binary; tests do not fall back to another repository executable. Windows
uses .exe by default and accepts make's extensionless target spelling too.
CLI tests can also be run directly, for example:
  python test/edge_regressions.py --binary ./wavegain
  python test/windows_files.py --binary ./wavegain.exe
Compiler/flag/linker arguments use the same shlex convention as the existing
filter/loudness drivers. Windows wrappers quote compiler paths automatically.

Core coverage (19 test groups on Windows)
-----------------------------------------
docs                  Generated help/manuals and option documentation.
support_contracts     Normal rejection vs crash/sanitizer error; Python -O guard.
filters               All-rate scalar/SIMD/mono/history equivalence and the
                      actual built CLI's output against a scalar CLI.
loudness              Three-way native reference/optimized energy/gating/peak checks,
                      including per-call peaks across tiny/64-frame boundaries,
                      signed zeros, changing magnitudes and float neighbors.
portability           PCM/float serialization, buffers and sparse large WAV/RF64 I/O.
wave64                Wave64 chunks, gain, formats, padding and sparse large I/O.
review_io             Allocation/read/write/seek/finalization fault injection.
feature_dsp           Decimator history bits, pass/stop bands and rate dither.
gain_kernel           Quantizer rounding/clamping/DC and non-finite input.
windows_errors        Win32 diagnostics and directory-link classification. [Windows]
output_publish        No-clobber publication; Windows post-preflight collision,
                      POSIX native/link/copy fallbacks and injected I/O failures.
command_regressions   Command-size, quoting, Win32 API and allocation faults. [Windows]
todo_regressions      DC/clipping/album peaks, progress, gain/undo and shared help.
review_regressions    RIFF/AIFF, metadata/padding, malformed input and preservation.
feature_regressions   Separate output, LUFS/album gates, true peak, high rates/dither.
followup_regressions  Six 2026-10-18 audit findings and supported format controls.
edge_regressions      Legacy malformed WAVs, strict stdin/seekable EOF recovery,
                      exact stdout WAV/AIFF/Wave64, original hard-link IDs,
                      400 ms LUFS and numeric-option/quantization boundaries.
windows_paths         Junction cycles and physical output containment. [Windows]
windows_files         Real sharing-lock/read-only failures retain original and
                      completed output; retry after release succeeds. [Windows]

Windows-only groups are explicitly SKIPped on POSIX. Hard-link/junction tests
need a supporting temporary filesystem; Windows directory tests use PowerShell.
The existing symlink tests require symlink creation rights on the test host.

Shared infrastructure
---------------------
test_support.py        CLI selection and strict process checking; reusable
                       DC/progress signals and diagnostic patterns.
audio_fixtures.py      Structural WAV/Wave64 builders and PCM extraction.
filter_regressions.c, filter_legacy_sse2.c and loudness_kernels.c are private
probes compiled by their Python drivers, not missing standalone gate entries.
The todo regression gate no longer imports the historical investigation script.

An expected CLI rejection must return 1. A signal, abort, Windows NTSTATUS or
sanitizer diagnostic fails the test even if the test expected invalid input.
The suite forces UBSan to halt, including when the compiler's default recovers.

Sanitizers and independent interoperability
------------------------------------------
  make check-sanitize
    Rebuilds with ASan/UBSan and explicit float-cast-overflow before core tests.
  python tools/check_windows.py --cc aarch64-w64-mingw32-clang --ubsan
    Native Windows UBSan plus float-cast-overflow, nonrecovering; uses O2 unless
    --optimized is also given. ASan is not implied by this switch.
  make check-interop
  python test/run_suite.py --suite interop --binary ./wavegain
    Four FFmpeg/ffprobe groups: Wave64, filters, RIFF/RF64 and features.
    Missing tools cause a clear failure before execution, not a claimed pass.
    Interoperability tests are optional and are not hidden inside make check.

Additional investigation and release checks
-------------------------------------------
pcm_equivalence.py compares complete outputs with a separately built prior CLI:
  python test/pcm_equivalence.py --reference before.exe --candidate wavegain.exe --output results.json
This differential corpus needs its reference binary and is not in the core
gate. Equality does not detect a defect shared by both binaries.
filter/loudness drivers expose additional benchmarks and prior-reference
comparisons; those modes are not throughput claims made by the core suite.

investigate_todo.py, investigate_followup.py and audit_command_bounds.c are
historical probes, outside the passing gate. The tiny repository fixtures
floating_point_exception.wav and memory_leak.wav are now exercised by edge
regressions on copies. outsox.wav is retained as a larger manual fixture;
it is not an independent audio-quality oracle or a core benchmark.

Native macOS ARM64 core, ASan/UBSan and FFmpeg interoperability checks pass;
dated measurement details are retained locally under ../local-research/.
Open validation includes official EBU conformance signals, real SMB/cloud and
ACL behavior, native GCC/MSVC, Linux execution, hardware x64 throughput,
and perceptual dither evaluation. See ../TODO for current open work.

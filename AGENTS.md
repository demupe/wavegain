# AGENTS.md

Guidance for coding agents (Claude Code, Codex, etc.) working in this repository.

WaveGain applies ReplayGain to wave files by rewriting the PCM data directly (optionally storing a `gain` RIFF chunk used only by `--undo-gain`). Plain C, built with a single Makefile; tests are Python 3 (stdlib only) plus small C test programs.

## Commands

- Build: `make` (= `release`; `release`/`debug`/`asan` run `clean` first, so they always rebuild everything). `make debug` enables `-Wall -Wextra -Wpedantic`; code should stay warning-clean there.
- Test: `make check` — delegates to `test/run_suite.py`, the shared POSIX/Windows inventory. It checks docs, filter/CLI and loudness equivalence, C probes (`-UNDEBUG`), and CLI regressions. See `test/README.txt`; use `--list` or repeat `--only NAME` to select tests.
- Interop tests: `make check-interop` (needs `ffmpeg`/`ffprobe`; uses `./wavegain` in the repo root).
- Single test: run its script with `--binary PATH`, or `python3 test/run_suite.py --only portability --cc gcc`. Interop entries use `--suite interop`. Compiler/CFLAGS/link inputs are supplied through the runner; C link lists live only there.
- Release build with filter verification: `make verified-release` (deletes the binary on mismatch; `VERIFY_SIMD=auto|0|1`).
- Regenerate docs: `make docs` (`python3 tools/generate_help.py`).
- There is no CI; `make check` is the gate.

## Architecture

- The Makefile compiles **every `*.c` in the root** into one binary (`$(wildcard *.c)`), so do not leave stray `.c` files there. Always built with `-D_FILE_OFFSET_BITS=64 -DHAVE_CONFIG_H`.
- `main.c`: option parsing (bundled `getopt.c`/`getopt1.c`) into `SETTINGS` (`main.h`), file list handling, and `process_files()` driving the two passes. `recurse.c` handles directory recursion (each folder = one album).
- `wavegain.c`: `get_gain()` (analysis pass, feeds `gain_analysis.c`), peak/gain selection (`corrected_peak()`, `selected_gain()`), and `write_gains()` (apply pass, with `dither.c`).
- `gain_analysis.c`: ReplayGain Yule/Butterworth filters, including paired-stereo SIMD paths. Results must be bit-identical to the reference — `check-filters` verifies this; FP contraction/FMA is intentionally disabled.
- `audio.c`: format sniffing in `open_audio_file()` (WAV/RIFF, RF64 input, AIFF, raw) and all output writers (`write_wav_header()`, `write_aiff_header()`, `output_to_PCM()`, `close_audio_file()`). `w64.c` implements Sony Wave64 read/write.
- Large-file rules (from v1.4.1):
  - WAV output over 4 GiB is still written, with sizes clamped to `0xFFFFFFFF` and a warning.
  - `--rf64` reserves a JUNK chunk and converts it to `ds64` only if the output exceeds 4 GiB. It needs seekable output and is ignored with `--stdout`.
  - Input with data size `0xFFFFFFFF` or a size past EOF is read to EOF when the input is seekable. Stdin input is strict.
  - RF64 input is read via `ds64`.
- Sizes use `Int64_t`/`Uint64_t`; PCM storage uses fixed-width 32-bit types (64-bit builds previously corrupted data).

## Conventions

- **CLI options and help text come from `doc/help.json`.** After changing options, run `make docs`. It regenerates `help_generated.h`, the help block in `README`, `debian/wavegain.1` and `doc/manual/index.html`. `make check-docs` fails if any of these are stale or if an option is undocumented.
  - A new option usually touches `doc/help.json`, `main.c`, `main.h`, `wavegain.c` and `audio.c`.
- Version string: `WAVEGAIN_VERSION` in `main.h`, also in `README`, `debian/wavegain.1` and `doc/manual/index.html`. Release commits are titled `release vX.Y.Z` and contain only that version bump.
- There is no CHANGELOG. Record user-visible output differences in the README's "Behavior changes" section, and record open items in `TODO`.
- Documentation index: `doc/README.txt`. Keep tracked documentation concise: usage, current design contracts and open work. Store detailed investigations, benchmarks/JSON evidence and review histories under gitignored `local-research/` (not distributed with the repository); keep report/data pairs together and preserve recorded measurements. Do not force-add local research or internal information. `doc/help.json` and `doc/manual/` retain their generator-defined paths.
- `debian/` packaging is outdated. Windows builds use the MSVC project in `mswindows/`.
- Code style: tab indentation, English comments. Keep the existing per-file license headers unchanged; license cleanup is an open `TODO` item.

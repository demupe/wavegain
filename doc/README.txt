WaveGain documentation
======================

../README                 Usage, formats, behavior changes and build commands.
../README.ja.md           Japanese usage and development guide.
../TODO                   Open development and validation work.
../test/README.txt        Test inventory and verification commands.
design/loudness-implementation.md
                          Measurement engine and precision contracts.
help.json                 Canonical CLI/help definitions.
manual/index.html         Generated CLI manual.
manual/legacy-notes.txt    Historical manual notes, not current CLI guidance.

Keep tracked documentation concise and focused on current behavior.
Detailed investigations, benchmark reports/JSON and review histories belong
in local-research/ at the repository root. That directory is gitignored,
local-only and not included in clones or releases. Keep report/data pairs
together and preserve original measurements when organizing local records.

After editing help.json, run make docs. Run make check-docs to verify the
README help, src/help_generated.h, man page and HTML manual are synchronized.

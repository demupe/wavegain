SHELL    = /bin/sh
ifneq ($(filter default undefined,$(origin CC)),)
CC       = gcc
endif

TARGET   = wavegain
CPPFLAGS += -D_FILE_OFFSET_BITS=64
DEFS     = -DHAVE_CONFIG_H
LIBS     = -lm
# MinGW: _FORTIFY_SOURCE (release builds) needs libssp. Link it statically so
# the .exe depends only on system DLLs (KERNEL32, msvcrt).
ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine 2>/dev/null)),)
LIBS    += -Wl,-Bstatic -lssp -Wl,-Bdynamic
endif
SOURCES := $(wildcard *.c)
HEADERS := $(wildcard *.h)
VENDOR := $(wildcard vendor/libebur128/*)

prefix   = /usr/local
bindir   = $(prefix)/bin

all: release

# DEBUG Build
# Add several compile- and run-time checks, warnings and preventions (Buffer Overflow, etc)
# -g3: Adds debugging information, level 3 (default for -g is level 2)
# -Og: Enables optimizations that do not interfere with debugging
# -Wall: Enables all the warnings about constructions that some users consider questionable, and that are easy to avoid
# -Wextra: Enables some extra warning flags that are not enabled by -Wall
debug: CFLAGS += -g3 -Og -Wall -Wextra -Wpedantic -D_FORTIFY_SOURCE=2

# RELEASE Build
# Stripped binary, maximum optimization
# -s: Strip executable (remove all symbol table and relocation information)
# -O2: Optimize level 2 and activate default FORTIFY_SOURCE=2 in most setups
# -D_FORTIFY_SOURCE=2: explicitely enable additional compile-time and run-time checks for several libc functions
release: CFLAGS += -s -O2 -D_FORTIFY_SOURCE=2

# Run clean before compilation even with parallel make.
debug release asan:
	$(MAKE) clean
	$(MAKE) $(TARGET) CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS)" DEFS="$(DEFS)" LDFLAGS="$(LDFLAGS)"

# Serialize clean/build even under -j, then verify before callers install.
# VERIFY_SIMD=auto accepts either filter backend once it matches the scalar
# reference; 1 requires the paired SIMD backend and 0 requires scalar.
VERIFY_SIMD ?= auto
ifeq ($(filter auto 0 1,$(VERIFY_SIMD)),)
$(error VERIFY_SIMD must be auto, 0 or 1)
endif
verified-release: override CFLAGS += -s -O2 -D_FORTIFY_SOURCE=2
verified-release: override CPPFLAGS += -D_FILE_OFFSET_BITS=64
verified-release:
	$(MAKE) clean
	$(MAKE) $(TARGET) CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" CFLAGS="$(CFLAGS)" DEFS="$(DEFS)" LDFLAGS="$(LDFLAGS)"
	python3 test/filter_regressions.py --cc "$(CC)" --cflags="$(CPPFLAGS) $(CFLAGS) $(DEFS)" --ldflags="$(LDFLAGS) $(LIBS)" $(if $(filter 0 1,$(VERIFY_SIMD)),--expect-simd $(VERIFY_SIMD)) --cli "./$(TARGET)" || { $(RM) "$(TARGET)"; exit 1; }

# ASan and UBSan build.
# Find some run-time errors that debug build misses.
# -U_FORTIFY_SOURCE: Disable FORTIFY_SOURCE, supposedly does not play well with ASAN
# -fsanitize=address,undefined: check memory safety and undefined behavior
# -fno-omit-frame-pointer: recommended to generate better stack traces on debugging
#: needed on some platforms to avoid run-time errors without using LD_PRELOAD
SANITIZER_FLAGS = -g3 -O1 -U_FORTIFY_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer
asan: CFLAGS += $(SANITIZER_FLAGS)

check-sanitize:
	$(MAKE) asan
	UBSAN_OPTIONS=halt_on_error=1 $(MAKE) check CFLAGS="$(CFLAGS) $(SANITIZER_FLAGS)"

$(TARGET): $(SOURCES) $(HEADERS) $(VENDOR) help_generated.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -o $(TARGET) $(SOURCES) $(LIBS)

install: $(TARGET)
	install -s -D $(TARGET) $(DESTDIR)$(bindir)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(TARGET)

check: $(TARGET) check-docs check-filters check-loudness
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -UNDEBUG -o test/portability test/portability.c w64.c $(LIBS)
	./test/portability
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -UNDEBUG -o test/wave64 test/wave64.c $(LIBS)
	./test/wave64
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -UNDEBUG -o test/review_io test/review_io.c w64.c wavegain.c gain_analysis.c dither.c misc.c loudness.c resample.c output.c $(LIBS)
	./test/review_io
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -UNDEBUG -o test/feature_dsp test/feature_dsp.c resample.c $(LIBS)
	./test/feature_dsp
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFS) $(LDFLAGS) -UNDEBUG -o test/output_publish test/output_publish.c $(LIBS)
	./test/output_publish
	$(RM) test/portability test/wave64 test/review_io test/feature_dsp test/output_publish
	python3 test/todo_regressions.py
	python3 test/review_regressions.py
	python3 test/feature_regressions.py

docs:
	python3 tools/generate_help.py

check-docs:
	python3 tools/generate_help.py --check

check-filters:
	python3 test/filter_regressions.py --cc "$(CC)" --cflags="$(CPPFLAGS) $(CFLAGS)"

check-loudness:
	python3 test/loudness_kernels.py --cc "$(CC)" --cflags="$(CPPFLAGS) $(CFLAGS)"

check-interop: $(TARGET)
	python3 test/wave64_interop.py
	python3 test/filter_interop.py
	python3 test/riff_interop.py
	python3 test/feature_interop.py

clean:
	rm -f $(TARGET) test/portability test/wave64 test/review_io test/feature_dsp test/output_publish

distclean: clean

.PHONY : all debug release verified-release asan install uninstall clean distclean check check-sanitize check-interop check-filters check-loudness docs check-docs

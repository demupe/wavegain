"""Shared CLI selection, process checks and DC/progress fixtures (stdlib only)."""
import argparse
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import wave

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / ('wavegain.exe' if os.name == 'nt' else 'wavegain')
PROGRESS = re.compile(r'This file\s+(\S+)% done\s*(?:All files\s+(\S+)% done)?')
GAIN = re.compile(r'([+-]?[\d.]+) dB\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)\s*\|\s*([\d.]+)')
SANITIZER_MARKERS = ('AddressSanitizer:', 'UndefinedBehaviorSanitizer:',
                     'LeakSanitizer:', 'MemorySanitizer:', 'ThreadSanitizer:', 'runtime error:')

if not __debug__:
    raise RuntimeError('Tests require assertions; remove -O/PYTHONOPTIMIZE.')


def resolve_binary(path):
    path = Path(path).resolve()
    if os.name == 'nt' and not path.is_file() and Path(str(path) + '.exe').is_file():
        path = Path(str(path) + '.exe')
    if not path.is_file():
        raise FileNotFoundError(f'Test binary does not exist: {path}')
    return path


def add_binary_argument(parser):
    parser.add_argument('--binary', type=Path, default=DEFAULT_BINARY,
                        help='Exact CLI to test (defaults to the repository build)')


def parse_binary(description):
    parser = argparse.ArgumentParser(description=description)
    add_binary_argument(parser)
    return resolve_binary(parser.parse_args().binary)


def check_result(result, success=True):
    def text(value):
        return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
    diagnostic = text(result.stderr) + '\n' + text(result.stdout)
    if any(marker in diagnostic for marker in SANITIZER_MARKERS):
        raise AssertionError(f'Sanitizer failure, including during expected rejection:\n{diagnostic}')
    expected = 0 if success else 1
    if result.returncode != expected:
        raise AssertionError(f'Expected CLI exit {expected}, got {result.returncode}: {result.args}\n{diagnostic}')
    return result


def run_cli(binary, args, *, success=True, input_data=None, cwd=None, text=False, timeout=30):
    return check_result(subprocess.run([str(binary), *map(str, args)], input=input_data,
                                      cwd=cwd, capture_output=True, text=text, timeout=timeout), success)


def write_fixture(path, rate, seconds, dc_pulses=False):
    data = bytearray()
    for i in range(round(rate * seconds)):
        if dc_pulses:
            value = -0.55 if i % (rate // 2) == 0 else .3 + .01 * math.sin(2 * math.pi * 440 * i / rate)
        else:
            value = .2 * math.sin(2 * math.pi * 440 * i / rate)
        data.extend(struct.pack('<hh', *([round(32767 * value)] * 2)))
    with wave.open(str(path), 'wb') as out:
        out.setparams((2, 2, rate, 0, 'NONE', 'PCM'))
        out.writeframes(data)

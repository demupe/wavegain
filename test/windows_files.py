"""Native locked/read-only replacement failures must retain original and completed output."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import struct
import tempfile
from audio_fixtures import wav
from test_support import parse_binary, run_cli


def main():
    binary = parse_binary(__doc__)
    if os.name != 'nt':
        print('SKIP windows_files: Windows only')
        return
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                   wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE)
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.SetFileAttributesW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD)
    kernel.SetFileAttributesW.restype = wintypes.BOOL
    options = ['-y', '-p', '-n', '--force', '-g', '-6']
    with tempfile.TemporaryDirectory(prefix='wavegain-windows-files-', ignore_cleanup_errors=True) as temp:
        folder = Path(temp)
        original = wav(struct.pack('<hh', 4000, -7000) * 4411)
        expected = folder / 'expected.wav'
        expected.write_bytes(original)
        run_cli(binary, [*options, expected])
        completed = expected.read_bytes()
        for case in ('locked', 'readonly'):
            source = folder / (case + '.wav')
            source.write_bytes(original)
            handle = None
            try:
                if case == 'locked':
                    # Allow reading but deny deletion/replacement by MoveFileEx.
                    handle = kernel.CreateFileW(str(source), 0x80000000, 1, None, 3, 0x80, None)
                    assert handle != wintypes.HANDLE(-1).value, ctypes.get_last_error()
                else:
                    assert kernel.SetFileAttributesW(str(source), 1), ctypes.get_last_error()
                result = run_cli(binary, [*options, source], success=False)
                assert b'completed output kept' in result.stderr, result.stderr
                assert source.read_bytes() == original
                retained = list(folder.glob(source.name + '.tmp_*'))
                assert len(retained) == 1 and retained[0].read_bytes() == completed, retained
            finally:
                if handle is not None and handle != wintypes.HANDLE(-1).value:
                    assert kernel.CloseHandle(handle), ctypes.get_last_error()
                assert kernel.SetFileAttributesW(str(source), 0x80), ctypes.get_last_error()
            # Retry after releasing the OS restriction succeeds exactly once.
            run_cli(binary, [*options, source])
            assert source.read_bytes() == completed
    print('Windows replacement: sharing locks/read-only failures preserve originals and completed output; retries pass')


if __name__ == '__main__':
    main()

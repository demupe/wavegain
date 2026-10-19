"""Ensure expected-rejection tests cannot silently accept crashes or sanitizer errors."""
import subprocess
import sys
import unittest
from test_support import ROOT, check_result


class ProcessChecks(unittest.TestCase):
    def result(self, code, stderr=b''):
        return subprocess.CompletedProcess(['synthetic CLI result'], code, b'', stderr)

    def test_regular_success_and_rejection(self):
        check_result(self.result(0))
        check_result(self.result(1, b'Invalid input'), success=False)

    def test_crashes_are_not_rejections(self):
        for code in (-8, -11, 3, 99, 0xc0000005):
            with self.subTest(code=code), self.assertRaises(AssertionError):
                check_result(self.result(code), success=False)

    def test_sanitizers_fail_even_if_exit_looks_expected(self):
        for code in (0, 1):
            for marker in (b'runtime error: out of range', b'ERROR: AddressSanitizer: heap-buffer-overflow'):
                with self.subTest(code=code, marker=marker), self.assertRaises(AssertionError):
                    check_result(self.result(code, marker), success=code == 0)

    def test_wrong_normal_exit_fails(self):
        for code, success in ((1, True), (0, False)):
            with self.subTest(code=code), self.assertRaises(AssertionError):
                check_result(self.result(code), success=success)

    def test_optimized_python_cannot_disable_the_gate_assertions(self):
        result = subprocess.run([sys.executable, '-O', str(ROOT / 'test/run_suite.py'), '--list'],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn('Tests require assertions', result.stderr)


if __name__ == '__main__':
    unittest.main()

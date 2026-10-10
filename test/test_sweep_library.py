#!/usr/bin/env python3
"""Consumer regressions for prefixed fatal boot diagnostics."""

from pathlib import Path
import sys
import unittest


_TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_TEST_DIR))
try:
    import sweep_library
finally:
    sys.path.pop(0)


class SweepDiagnosticTests(unittest.TestCase):
    def test_prefixed_fatal_traps_at_record_boundaries(self):
        for name in ('addr', 'undf', 'pabt', 'dabt'):
            for separator in ('', '\r', '\n', '\r\n'):
                with self.subTest(trap=name, separator=separator):
                    trap = 'opera-boot: TRAP %s lr=00018004' % name
                    self.assertIsNotNone(
                        sweep_library.FAIL_RE.search(separator + trap))
                    log = 'opera-boot: boot 00018000\r\n' + trap
                    self.assertIsNotNone(sweep_library.FAIL_RE.search(log))

    def test_fatal_trap_requires_record_boundary_and_complete_address(self):
        cases = (
            'field=opera-boot: TRAP undf lr=00018004',
            '\r\nopera-boot: TRAP undf lr=0001800',
            '\r\nopera-boot: TRAP undf lr=0001800G',
            '\r\nopera-boot: TRAP unknown lr=00018004',
        )
        for log in cases:
            with self.subTest(log=log):
                self.assertIsNone(sweep_library.FAIL_RE.search(log))


if __name__ == '__main__':
    unittest.main()

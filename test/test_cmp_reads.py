#!/usr/bin/env python3
"""Consumer regressions for CDCMD recovery, sector ordering, and handoff."""

import importlib.util
from pathlib import Path
import tempfile
import unittest


_SPEC = importlib.util.spec_from_file_location(
    'cmp_reads', Path(__file__).resolve().with_name('cmp_reads.py'))
cmp_reads = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(cmp_reads)


class ReadComparatorTests(unittest.TestCase):
    def parse_log(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'run.log'
            path.write_text(text, encoding='utf-8')
            return cmp_reads.reads(path)

    def test_reads_preserve_lba_and_24_bit_count(self):
        text = (
            '@CDCMD 0 10 00 02 39 00 00 01\n'
            'CDCMD 1 02 00 00 00 00 00 00\n'
            'CDCMD 2 10 27 1D 2F 01 02 03\n'
        )
        self.assertEqual(self.parse_log(text), [(207, 1), (177722, 66051)])

    def test_reads_recover_plaintext_interruptions(self):
        cases = (
            ('inside_byte',
             '@CDCMD 0 10 00 02 3opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n9 00 00 01\n', (207, 1)),
            ('between_bytes',
             'CDCMD 0 10 00 02 opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n39 00 00 01\n', (207, 1)),
            ('after_marker',
             '@CDCMDopera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n 0 10 27 1D 2F 00 00 05\n',
             (177722, 5)),
        )
        for name, text, expected in cases:
            with self.subTest(boundary=name):
                self.assertEqual(self.parse_log(text), [expected])

    def test_command_bytes_preserve_every_byte_across_prefixed_records(self):
        command = '10 27 1D 2F 01 02 03'
        diagnostic = (
            'opera-boot: disc identifier="A\\x0A\\x22\\x5C\\xFF"\r\n'
            'opera-boot: tag splash missing (optional)\r\n'
        )
        expected = [0x10, 0x27, 0x1D, 0x2F, 0x01, 0x02, 0x03]
        for split in range(1, len(command)):
            with self.subTest(split=split):
                record = command[:split] + diagnostic + command[split:] + '\n'
                self.assertEqual(
                    cmp_reads.command_bytes(record, 'run.log', 0), expected)

    def test_reads_reject_invalid_interruption_boundaries(self):
        cases = (
            ('missing_byte_resume',
             'CDCMD 0 10 00 02 3opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n'),
            ('corrupt_byte_resume',
             'CDCMD 0 10 00 02 3opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\nG 00 00 01\n'),
            ('short_byte_resume',
             'CDCMD 0 10 00 02 3opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n9 00 01\n'),
            ('unterminated_byte_resume',
             'CDCMD 0 10 00 02 3opera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n9 00 00 01'),
            ('missing_header_resume',
             'CDCMDopera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n'),
            ('corrupt_header_resume',
             'CDCMDopera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n 0 10 27 1D GG 00 00 05\n'),
        )
        neighbor = 'CDCMD 1 10 00 02 3A 00 00 01\n'
        for name, text in cases:
            with self.subTest(boundary=name):
                with self.assertRaisesRegex(ValueError, 'malformed/interleaved'):
                    self.parse_log(text + neighbor)

    def test_reads_reject_sequence_gaps_without_inference(self):
        first = 'CDCMD 0 10 00 02 38 00 00 01\n'
        cases = (
            ('ordinary_header', 'CDCMD 2 10 00 02 3A 00 00 01\n'),
            ('resumed_header',
             'CDCMDopera-boot: handoff target=disc OS kernel\r\n'
             'opera-boot: boot 00018000\r\n 2 10 00 02 3A 00 00 01\n'),
        )
        for name, text in cases:
            with self.subTest(boundary=name):
                with self.assertRaisesRegex(ValueError, 'expected 1, got 2'):
                    self.parse_log(first + text)

    def test_sector_comparison_accepts_repartitioned_bursts(self):
        seven_then_eight = [(175205, 7), (175212, 8)]
        eight_then_seven = [(175205, 8), (175213, 7)]
        self.assertIsNone(cmp_reads.unmatched_sector(
            seven_then_eight, eight_then_seven))

    def test_sector_comparison_reports_first_unmatched(self):
        gold = [(175205, 8), (175213, 7)]
        cases = (
            ('reordered', [(175212, 8), (175205, 7)], (1, 0, 175205)),
            ('uncovered', [(175205, 16)], (0, 15, 175220)),
            ('repeated', [(175205, 8), (175212, 1), (175213, 7)],
             (1, 0, 175212)),
        )
        for name, candidate, expected in cases:
            with self.subTest(sectors=name):
                self.assertEqual(
                    cmp_reads.unmatched_sector(candidate, gold), expected)

    def test_retail_boundary_returns_unique_complete_tail_end(self):
        tail = [(300, 1), (301, 1), (900, 1), (901, 1)]
        gold = ([(150, 1), (151, 1), (152, 1)] + tail
                + [(999, 1), (1000, 4)] + tail)
        self.assertEqual(cmp_reads.retail_boot_end(gold, tail), 7)

    def test_retail_boundary_rejects_incomplete_or_ambiguous_tails(self):
        label = [(150, 1), (151, 1)]
        tail = [(300, 1), (301, 1), (900, 1), (901, 1)]
        burst = [(1000, 4)]
        cases = (
            ('missing', label + [(999, 1)] + burst,
             '0 complete component tails'),
            ('truncated', label + tail[:-1] + burst,
             '0 complete component tails'),
            ('duplicate', label + tail + tail + burst,
             '2 complete component tails'),
            ('after_first_multiblock', label + burst + tail,
             '0 complete component tails'),
            ('no_multiblock', label + tail,
             'partial: no filesystem multi-block read'),
        )
        for name, gold, message in cases:
            with self.subTest(boundary=name):
                with self.assertRaisesRegex(ValueError, message):
                    cmp_reads.retail_boot_end(gold, tail)


if __name__ == '__main__':
    unittest.main()

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gba_benchmark_report import summarize


class ReportTests(unittest.TestCase):
    def test_unequal_intervals(self):
        meta, result = summarize('''# schema=gba-benchmark-v3-1
# mode=clean
interval_s,frames,drawn,max_frame_ms,frames_gt33ms,execute_ms
1.0,60,60,17,0,600
0.5,15,10,40,2,150
# stop=menu
''')
        self.assertEqual(result['fps'], 50)  # not (60+30)/2
        self.assertAlmostEqual(result['drawn_fps'], 70 / 1.5)
        self.assertEqual(result['execute_ms_per_frame'], 10)
        self.assertEqual(result['max_ms'], 40)
        self.assertEqual(meta['stop'], 'menu')

    def test_reject_empty_or_legacy(self):
        for text in ('', '# schema=gba-benchmark-v3-1\ninterval_s,frames\n'):
            with self.assertRaises(ValueError):
                summarize(text)


if __name__ == '__main__':
    unittest.main()

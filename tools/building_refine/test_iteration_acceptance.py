import unittest
from merge_city_pass import window_layout_regressions

class IterationAcceptanceTest(unittest.TestCase):
    def test_large_window_loss_retains_previous_but_alias_merge_does_not(self):
        old=[dict(building='a',counts={'window':100}),dict(building='b',counts={'window':100})]
        new=[dict(building='a',counts={'window':80}),dict(building='b',status='combined_into_building',counts={})]
        self.assertEqual(set(window_layout_regressions(old,new)),{'a'})
    def test_small_detection_changes_do_not_block_iteration(self):
        old=[dict(building='a',counts={'window':100})]
        self.assertFalse(window_layout_regressions(old,[dict(building='a',counts={'window':98})]))

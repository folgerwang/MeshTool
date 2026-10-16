import unittest
import numpy as np
from window_detail import measure, geometry
from solid_city import audit_shell


class WindowDetailTest(unittest.TestCase):
    def test_captured_cross_members(self):
        image = np.full((100, 80, 3), 45, np.uint8)
        image[:, 38:42] = 180
        image[58:62, :] = 180
        members = measure(image, 2., 2.5)
        self.assertEqual({m['axis'] for m in members}, {'x', 'y'})
        self.assertAlmostEqual(next(m['position'] for m in members if m['axis']=='x'), .5, places=2)
        self.assertAlmostEqual(next(m['position'] for m in members if m['axis']=='y'), .4, places=2)
        points = geometry(2., 2.5, 'arched', members, .07)
        self.assertTrue(np.isfinite(points).all())
        self.assertGreater(len(points), 0)
        self.assertLess(points[:,2].max(), 0)

    def test_no_grid_invented_on_flat_glass_or_broad_reflection(self):
        image = np.full((100, 80, 3), 45, np.uint8)
        self.assertEqual(measure(image, 2., 2.5), [])
        image[:,20:60] = 130
        self.assertEqual(measure(image, 2., 2.5), [])

    def test_short_scratch_is_not_a_mullion(self):
        image = np.full((100,80,3),45,np.uint8)
        image[30:45,38:42] = 200
        self.assertEqual(measure(image,2.,2.5),[])

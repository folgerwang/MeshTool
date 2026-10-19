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

    def test_dark_frames_and_bright_frames_are_recovered(self):
        image=np.full((100,80,3),140,np.uint8)
        image[:,38:42]=30
        image[58:62,:]=30
        members=measure(image,2.,2.5,both_polarities=True)
        self.assertEqual({m['axis'] for m in members},{'x','y'})
        self.assertEqual(len(members),2)
        self.assertEqual(measure(np.full_like(image,100),2.,2.5,both_polarities=True),[])

    def test_frame_color_comes_from_measured_bar_not_light_wall(self):
        from window_detail import member_color
        image=np.full((100,80,3),170,np.uint8)
        image[:,38:42]=[35,40,45]
        members=measure(image,2.,2.5,both_polarities=True)
        np.testing.assert_array_equal(member_color(image,2.,2.5,members),[35,40,45])

    def test_dark_scratch_and_broad_shadow_are_not_frames(self):
        image=np.full((100,80,3),150,np.uint8)
        image[30:45,38:42]=25
        self.assertEqual(measure(image,2.,2.5,both_polarities=True),[])
        image[:]=150;image[:,20:60]=25
        self.assertEqual(measure(image,2.,2.5,both_polarities=True),[])

    def test_arch_crossbar_clipping_has_no_degenerate_sides(self):
        from types import SimpleNamespace
        members=[dict(axis='x',position=.1346153846153846,width_m=.08),dict(axis='y',position=.859375,width_m=.08)]
        points=geometry(2.08,2.56,'arched',members,.07)
        points[:,:2]*=[2.08,2.56]
        mesh=SimpleNamespace(vertices=points,translation=np.zeros(3),draw_calls=[(4,np.arange(len(points)))])
        self.assertEqual(audit_shell([mesh]),dict(boundary_edges=0,nonmanifold_edges=0,degenerate_triangles=0))

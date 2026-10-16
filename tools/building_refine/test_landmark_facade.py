import unittest
import numpy as np
from shapely.geometry import box
import mtscene
from landmark_facade import measured_openings, extrusion, masonry
from solid_city import audit_shell


class LandmarkFacadeTest(unittest.TestCase):
    def test_glass_material_is_shared_and_only_assigned_to_glazing(self):
        from building_rules import material_for_part
        for role in ('glazing','glass','roof_glass'):
            self.assertEqual(material_for_part(role),mtscene.MAT_GLASS)
        for role in ('frame','wall','trim','door_leaf','roof_metal','roof_stone'):
            self.assertEqual(material_for_part(role),mtscene.MAT_CAPTURED)

    def test_shared_roof_frame_has_raised_closed_steps(self):
        from building_rules import roof_frame
        for size in (5,20):
            pieces=roof_frame(box(0,0,size,size),10,width=.3,rise=.24)
            self.assertEqual(len(pieces),2)
            self.assertAlmostEqual(max(p[:,2].max() for p in pieces),10.24)
            for points in pieces:
                mesh=mtscene.Mesh(0,0,np.zeros(3),points,None,None,[(4,np.arange(len(points),dtype=np.uint32))])
                self.assertFalse(any(audit_shell([mesh]).values()))

    def test_capture_material_preserves_wall_and_removes_window_image(self):
        from landmark_facade import captured_wall_material
        image=np.full((100,100,3),190,np.uint8)
        image[30:70,30:70]=20
        image[80:85,10:15]=(180,179,175)
        openings=[dict(bounds=[3,3,7,7])]
        result,mask=captured_wall_material(image,openings,.1)
        np.testing.assert_array_equal(result[mask==0],image[mask==0])
        self.assertGreater(result[40:60,40:60].mean(),170)
        blank,_=captured_wall_material(np.zeros_like(image),[],.1)
        self.assertGreater(blank.mean(),150)

    def test_cornice_is_not_classified_as_windows(self):
        for face in (0,7,17):
            openings=measured_openings(face)
            self.assertTrue(openings)
            self.assertTrue(all(o['bounds'][3]<30 for o in openings))
            self.assertTrue(all(o['kind']=='window' for o in openings))
        self.assertEqual(measured_openings(4),[])

    def test_observed_bay_counts_and_whole_grouped_windows(self):
        for face,count in ((0,7),(7,7),(17,4)):
            openings=measured_openings(face)
            for role in ('ground_arch','tall_arch','triple'):
                self.assertEqual(sum(o['role']==role for o in openings),count)

    def test_trim_ring_is_closed(self):
        p=box(0,0,3,4).difference(box(.2,.2,2.8,3.8))
        vertices=extrusion(p,.02,.2)
        mesh=mtscene.Mesh(0,0,np.zeros(3),vertices,None,None,[(4,np.arange(len(vertices),dtype=np.uint32))])
        self.assertFalse(any(audit_shell([mesh]).values()))

    def test_roof_beams_are_closed_and_roof_has_measured_extent(self):
        from landmark_roof import beam,geometry,ORIGIN,U,V
        points=beam([0,0,0],[2,3,4])
        mesh=mtscene.Mesh(0,0,np.zeros(3),points,None,None,[(4,np.arange(len(points),dtype=np.uint32))])
        self.assertFalse(any(audit_shell([mesh]).values()))
        parts=geometry();world=np.concatenate([p for p,role in parts])
        self.assertTrue(np.isfinite(world).all())
        self.assertAlmostEqual(world[:,2].max(),59.88,places=2)
        glass=np.concatenate([p for p,role in parts if role=='roof_glass'])
        local=np.c_[(glass-ORIGIN)@U,(glass-ORIGIN)@V,glass[:,2]]
        dome=local[local[:,0]<20.4]
        self.assertGreater(len(dome),100)
        self.assertAlmostEqual(dome[:,2].max(),58.4,places=2)

    def test_masonry_material_is_reproducible_and_subtle(self):
        a,b=masonry(),masonry()
        np.testing.assert_array_equal(a,b)
        self.assertLess(np.std(a[:,:,0]),5)


if __name__=='__main__':unittest.main()

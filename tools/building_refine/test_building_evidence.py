import unittest
import numpy as np
import building_evidence as evidence
import facade
from test_pcg import fixture, fingerprint
from test_base_recovery import mesh,wall


class BuildingEvidenceTest(unittest.TestCase):
    def test_complete_building_and_partial_planar_facade(self):
        self.assertTrue(evidence.check([fixture()[0].groups[0].meshes[0]])['accepted'])
        self.assertTrue(evidence.check([mesh(wall(0,12,0,20),0)])['accepted'])

    def test_ground_slopes_single_triangles_and_empty_mesh_are_rejected(self):
        p=np.array(wall(0,12,0,20)).reshape(-1,3,3)
        self.assertFalse(evidence.check_triangles(p[:1])['accepted'])
        p=p[:,:,[0,2,1]];p[:,:,2]=p[:,:,0]*.8
        self.assertFalse(evidence.check_triangles(p)['accepted'])
        self.assertFalse(evidence.check([])['accepted'])

    def test_sparse_trapezoid_does_not_count_as_a_rectangular_facade(self):
        p=np.array([[[0,0,0],[12,0,0],[12,0,20]],[[0,0,0],[12,0,20],[0,0,4]]],float)
        self.assertFalse(evidence.check_triangles(p)['accepted'])

    def test_foliage_like_radial_surface_is_not_a_facade(self):
        p=[]
        for i in range(16):
            a=i*np.pi/8;b=(i+1)*np.pi/8
            p.append([[0,0,18],[5*np.cos(a),5*np.sin(a),0],[5*np.cos(b),5*np.sin(b),0]])
        self.assertFalse(evidence.check_triangles(np.array(p))['accepted'])

    def test_rejected_object_is_untouched_including_previous_refinement(self):
        s=fixture();g=s[0].groups[0];bad=mesh([[0,0,0],[12,0,0],[12,0,20]],0)
        bad.model_variant=1;g.meshes[0]=bad
        previous=mesh(wall(0,12,0,20),0);previous.model_variant=2;g.meshes.append(previous)
        before=[fingerprint(m) for m in g.meshes]
        self.assertEqual(facade.apply(s,(0,0,0)),(0,1))
        self.assertEqual(before,[fingerprint(m) for m in g.meshes])

if __name__=='__main__':unittest.main()

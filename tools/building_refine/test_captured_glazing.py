import copy
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
import mtscene
import raster
from captured_glazing import covered_triangles, transfer, pane_components, complete_panes


class CapturedGlazingTest(unittest.TestCase):
    def test_partial_capture_keeps_whole_pane_previous_material(self):
        points=np.array([[[0,0,0],[1,0,0],[1,0,1]],[[0,0,0],[1,0,1],[0,0,1]],[[3,0,0],[4,0,0],[3,0,1]]])
        components=pane_components(points)
        np.testing.assert_array_equal(complete_panes(np.array([0,2]),components),[2])
        np.testing.assert_array_equal(complete_panes(np.array([0,1,2]),components),[0,1,2])

    def test_missing_pane_interior_is_not_hidden_by_observed_vertices(self):
        mask=np.ones((50,50),bool);mask[10:30,10:30]=False
        triangles=np.array([[[2,2],[45,2],[2,45]],[[35,35],[45,35],[35,45]]])
        self.assertEqual(covered_triangles(triangles,mask,.98).tolist(),[False,True])

    def test_outside_capture_is_rejected(self):
        self.assertFalse(covered_triangles(np.array([[[-1,0],[4,0],[0,4]]]),np.ones((8,8),bool),.98)[0])

    def test_transfer_preserves_geometry_material_and_unsupported_panes(self):
        vertices=np.array([[.2,0,.2],[1.8,0,.2],[.2,0,1.8]],np.float32)
        mesh=mtscene.Mesh(0,0,np.zeros(3),vertices,np.zeros((3,2),np.float32),None,[(4,np.arange(3,dtype=np.uint32))],1,0)
        catalog=dict(faces=[dict(face_id=0,origin=[0,0,0],u=[1,0,0],v=[0,0,1],normal=[0,-1,0],size=[2,2])],semantic_meshes=[dict(local_mesh_index=0,part='glazing')])
        source=raster.SceneTris(vertices[None],np.zeros((1,3,2)),np.array([0]),np.array([0]),np.array([0]))
        rgb=np.full((25,25,3),80,np.uint8)
        for supported in (False,True):
            with tempfile.TemporaryDirectory() as directory, patch('refine.project_face',return_value=(rgb,np.zeros((25,25)),np.full((25,25),supported))):
                textures=[None]
                meshes,report=transfer([mesh],textures,copy.deepcopy(catalog),source,[],directory)
                self.assertEqual(len(meshes),1)
                np.testing.assert_array_equal(meshes[0].vertices,mesh.vertices)
                np.testing.assert_array_equal(meshes[0].draw_calls[0][1],mesh.draw_calls[0][1])
                self.assertEqual(meshes[0].material,1)
                self.assertEqual(report['faces'][0]['mapped_glass_triangles'],int(supported))
                self.assertEqual(meshes[0].tex_index,int(supported))
                if supported:self.assertGreater(np.ptp(meshes[0].uvs),0)


if __name__=='__main__':unittest.main()

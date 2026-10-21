import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
from shapely.geometry import Polygon
import mtscene,raster
from capture_tiles import capture_faces,refine_building
from solid_city import shell,audit_shell


def texture(image):
    return mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,
                          [(image.shape[1],image.shape[0],raster.encode_dxt1(image))])


def tiled_wall():
    points=[]
    for y in range(12):
        for x in range(12):
            q=np.array([[x,0,y],[x+1,0,y],[x+1,0,y+1],[x,0,y+1]],float)
            points.extend(q[[0,1,2,0,2,3]])
    points=np.array(points,np.float32)
    uv=np.c_[points[:,0]/12,1-points[:,2]/12].astype(np.float32)
    return mtscene.Mesh(0,0,np.zeros(3),points,uv,None,[(4,np.arange(len(points),dtype=np.uint32))])


class FacadeIterationTest(unittest.TestCase):
    def test_tilted_blank_shell_preserves_vertices_and_closed_seams(self):
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        for mesh in meshes:mesh.vertices[:,0]+=.18*mesh.vertices[:,2]
        tex=texture(np.full((256,256,3),175,np.uint8))
        before=audit_shell(meshes)
        with tempfile.TemporaryDirectory() as path:
            result,_=refine_building(meshes,mtscene.SceneObject('tilt',3),[tex,tex],Path(path),clean=True)
            self.assertEqual(audit_shell(result),before)
            original={tuple(p) for mesh in meshes for p in mesh.vertices.astype(float)+mesh.translation}
            generated={tuple(p) for mesh in result for p in mesh.vertices.astype(float)+mesh.translation}
            self.assertEqual(generated,original)
            catalog=json.loads((Path(path)/'tiles.json').read_text())
            self.assertTrue(all(f['geometry_policy']=='preserve_source_triangles' for f in catalog['faces']))

    def test_tilted_wall_frame_is_orthonormal(self):
        mesh=tiled_wall();mesh.vertices[:,1]=mesh.vertices[:,2]*.2
        for face,_ in capture_faces(mesh):
            frame=np.stack([face.u,face.v,face.normal],axis=1)
            np.testing.assert_allclose(frame.T@frame,np.eye(3),atol=1e-7)

    def test_window_layout_spans_coplanar_capture_tiles(self):
        image=np.full((256,256,3),180,np.uint8)
        for y in (30,100,170):
            for x in (25,85,145,205):image[y:y+35,x:x+20]=(30,45,60)
        mesh=tiled_wall()
        self.assertEqual(len(list(capture_faces(mesh))),1)
        with tempfile.TemporaryDirectory() as path:
            result,report=refine_building([mesh],mtscene.SceneObject('tiled',3),[texture(image)],Path(path),clean=True)
            self.assertEqual(report['counts']['window'],12)
            catalog=json.loads((Path(path)/'tiles.json').read_text())
            self.assertEqual(len(catalog['faces']),1)
            self.assertIsNotNone(catalog['faces'][0]['pattern']['floor'])
            self.assertEqual(audit_shell(result),audit_shell([mesh]))

    def test_uv_discontinuities_are_not_merged(self):
        mesh=tiled_wall();mesh.uvs[6:12,0]+=.1
        self.assertGreater(len(list(capture_faces(mesh))),1)

    def test_existing_glass_and_interiors_remain_semantic_parts(self):
        mesh=tiled_wall();mesh.material=mtscene.MAT_GLASS
        interior=tiled_wall();interior.material=mtscene.MAT_INTERIOR;interior.vertices[:,1]=.1
        tex=texture(np.full((256,256,3),(45,61,72),np.uint8))
        with tempfile.TemporaryDirectory() as path:
            result,_=refine_building([mesh,interior],mtscene.SceneObject('existing',3),[tex],Path(path),clean=True)
            self.assertEqual([m.material for m in result],[mtscene.MAT_GLASS,mtscene.MAT_INTERIOR])
            self.assertTrue(all(m.tex_index>0 for m in result))
            catalog=json.loads((Path(path)/'tiles.json').read_text())
            self.assertEqual(catalog['retained_native_parts'],{'glazing':1,'interior':1})
            self.assertEqual(catalog['faces'],[])

    def test_facade_only_run_preserves_roof_capture(self):
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        tex=texture(np.full((256,256,3),175,np.uint8))
        with tempfile.TemporaryDirectory() as path:
            result,_=refine_building(meshes,mtscene.SceneObject('roofs',3),[tex,tex],Path(path),clean=True,preserve_roofs=True)
            catalog=json.loads((Path(path)/'tiles.json').read_text())
            roof=next(result[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part']=='roof')
            self.assertEqual(roof.tex_index,meshes[1].tex_index)
            np.testing.assert_array_equal(roof.vertices,meshes[1].vertices)
            np.testing.assert_array_equal(roof.uvs,meshes[1].uvs)
            self.assertEqual(audit_shell(result),audit_shell(meshes))


if __name__=='__main__':unittest.main()

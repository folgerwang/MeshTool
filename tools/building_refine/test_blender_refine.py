import os
import unittest
import numpy as np
import blender_refine
import mtscene


def quad_mesh(oid, uv_scale):
    # Two triangles of a 4 m x 4 m wall; uv_scale 0 collapses the texture.
    v=np.array([[0,0,0],[4,0,0],[4,0,4],[0,0,4]],np.float32)
    uv=np.array([[0,0],[1,0],[1,1],[0,1]],np.float32)*uv_scale
    return mtscene.Mesh(0,oid,np.array([10.,20.,30.]),v,uv,None,[(4,np.array([0,1,2,0,2,3],np.uint32))])


class BlenderRefineTest(unittest.TestCase):
    def test_smeared_faces_are_flagged_against_building_density(self):
        textures=[mtscene.Texture(mips=[(64,64,b'')])]
        meshes=[quad_mesh(0,1.)]*20+[quad_mesh(0,1e-4)]
        data=blender_refine.triangles(meshes,textures)
        self.assertEqual(len(data['pos']),42)
        np.testing.assert_array_equal(data['bad'][:40],0)
        np.testing.assert_array_equal(data['bad'][40:],blender_refine.SMEARED)

    def test_rebuild_keeps_mesh_texture_and_returns_world_positions(self):
        textures=[mtscene.Texture(mips=[(64,64,b'')])]
        meshes=[quad_mesh(0,1.),quad_mesh(0,.5)]
        data=blender_refine.triangles(meshes,textures)
        center=data['pos'].reshape(-1,3).mean(0)
        out=dict(pos=data['pos'][1:]-center,uv=data['uv'][1:],src=data['src'][1:])
        new=blender_refine.rebuild(meshes,out,center)
        self.assertEqual([len(m.draw_calls[0][1]) for m in new],[3,6])
        self.assertTrue(all(m.model_variant==2 and m.tex_index==0 for m in new))
        np.testing.assert_allclose(new[1].vertices.astype(float)+new[1].translation,data['pos'][2:].reshape(-1,3),atol=1e-4)
        np.testing.assert_allclose(new[1].uvs,data['uv'][2:].reshape(-1,2))

    @unittest.skipUnless(os.path.exists(blender_refine.BLENDER),'Blender not installed')
    def test_blender_drops_fragment_and_keeps_wall(self):
        textures=[mtscene.Texture(mips=[(64,64,b'')])]
        wall=quad_mesh(0,1.)
        speck=quad_mesh(0,1.);speck.vertices=speck.vertices*.2+np.float32([0,-5,0])
        group=mtscene.Group(textures,[wall,speck],[mtscene.SceneObject('building_1',3)])
        batches=[mtscene.Batch(False,False,False,(0.,0.,0.),[group])]
        blender_refine.run(batches,log=lambda *a:None)
        refined=[m for m in group.meshes if m.model_variant==2]
        self.assertEqual(sum(len(m.draw_calls[0][1]) for m in refined),6)
        self.assertEqual(len([m for m in group.meshes if m.model_variant==1]),2)


if __name__=='__main__':unittest.main()

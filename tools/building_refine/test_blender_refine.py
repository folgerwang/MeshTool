import json
import os
import subprocess
import tempfile
import unittest
import numpy as np
import blender_refine
import captured_glass
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

    def test_wall_planes_finds_outward_wall(self):
        q=np.array([[[0,0,0],[10,0,0],[10,0,10]],[[0,0,0],[10,0,10],[0,0,10]]],float)
        planes=captured_glass.wall_planes(q,min_area=20)
        self.assertEqual(len(planes),1)
        np.testing.assert_allclose(abs(planes[0]['n'][1]),1.)
        self.assertEqual(sorted(planes[0]['faces']),[0,1])

    def test_panes_found_on_a_window_grid_and_not_on_a_blank_wall(self):
        pitch=.08;image=np.full((300,500,3),150,np.uint8);mask=np.ones((300,500),bool)
        self.assertEqual(captured_glass.panes(image,mask,pitch),[])
        for row in (40,130,220):
            for col in range(40,460,80):
                image[row:row+45,col:col+30]=70
        found=captured_glass.panes(image,mask,pitch)
        self.assertEqual(len(found),18)
        self.assertIn((40,40,70,85),found)

    def test_frame_ring_stops_halfway_to_the_next_pane(self):
        (a,b)=captured_glass.framed([(0,0,1,2),(1.06,0,2,2)])
        self.assertAlmostEqual(a[6],1.03)
        self.assertAlmostEqual(b[4],1.03)
        self.assertAlmostEqual(a[4],-captured_glass.FRAME)

    @unittest.skipUnless(os.path.exists(blender_refine.BLENDER),'Blender not installed')
    def test_blender_drops_fragment_and_keeps_wall(self):
        textures=[mtscene.Texture(mips=[(64,64,b'')])]
        wall=quad_mesh(0,1.)
        speck=quad_mesh(0,1.);speck.vertices=speck.vertices*.2+np.float32([0,-5,0])
        group=mtscene.Group(textures,[wall,speck],[mtscene.SceneObject('building_1',3)])
        batches=[mtscene.Batch(False,False,False,(0.,0.,0.),[group])]
        blender_refine.run(batches,log=lambda *a:None,windows=False)
        refined=[m for m in group.meshes if m.model_variant==2]
        self.assertEqual(sum(len(m.draw_calls[0][1]) for m in refined),6)
        self.assertEqual(len([m for m in group.meshes if m.model_variant==1]),2)


    @unittest.skipUnless(os.path.exists(blender_refine.BLENDER),'Blender not installed')
    def test_blender_builds_recessed_window_with_glass(self):
        job=tempfile.mkdtemp()
        wall=quad_mesh(0,1.);d=blender_refine.triangles([wall],[mtscene.Texture(mips=[(64,64,b'')])])
        center=d['pos'].reshape(-1,3).mean(0);d['pos']=d['pos']-center
        np.savez(os.path.join(job,'b_in.npz'),**d)
        n=np.cross(d['pos'][0,1]-d['pos'][0,0],d['pos'][0,2]-d['pos'][0,0]);n=n/np.linalg.norm(n)
        u=np.array([-n[1],n[0],0.]);o=d['pos'].reshape(-1,3).min(0) if u[0]>0 else d['pos'].reshape(-1,3).max(0)*[1,0,0]+d['pos'].reshape(-1,3).min(0)*[0,1,1]
        plane=dict(origin=list(o),u=list(u),v=[0,0,1],n=list(n),size=[4,4],rects=[[1,1,3,3,.9,.9,3.1,3.1]])
        with open(os.path.join(job,'b_glass.json'),'w') as f:json.dump([plane],f)
        script=os.path.join(os.path.dirname(blender_refine.__file__),'blender_cleanup.py')
        subprocess.run([blender_refine.BLENDER,'-b','--factory-startup','--python',script,'--',job],check=True,stdout=subprocess.DEVNULL)
        out=np.load(os.path.join(job,'b_out.npz'));mat=out['mat']
        self.assertEqual(json.load(open(os.path.join(job,'report.json')))['b']['panes'],1)
        self.assertEqual((mat==1).sum(),2)
        depth=(out['pos'].reshape(-1,3)-o)@n
        pane=((out['pos'][mat==1].reshape(-1,3)-o)@n)
        np.testing.assert_allclose(pane,-.15,atol=1e-3)
        self.assertGreaterEqual(depth[np.repeat(mat==0,3)].min(),-1e-3)


if __name__=='__main__':unittest.main()

"""Procedural tree bounds, terrain placement, original safety and persistence."""
import copy
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
import mtscene
import trees
from test_pcg import model, mesh_from_model, fingerprint


def fixture():
    captured = mesh_from_model(model(),0)
    captured.vertices *= np.array([.7,.6,1.])
    captured.translation = np.array([10.,20.,3.])
    captured.tex_index = 0
    captured.uvs = np.zeros((len(captured.vertices),2),np.float32)
    texture = trees.palette()[0]
    ground = mtscene.Mesh(0xffffffff,2,np.zeros(3),np.array([[0,0,0],[40,0,0],[40,50,0],[0,0,0],[40,50,0],[0,50,0]],np.float32),
                          None,None,[(4,np.arange(6,dtype=np.uint32))])
    neighbor = mesh_from_model(model(30),1)
    other = copy.deepcopy(captured);other.object_id=3;other.translation[0]+=20
    return mtscene.Scene([mtscene.Batch(False,True,True,(-122.4,37.7),
        [mtscene.Group([texture],[captured,neighbor,ground,other],
            [mtscene.SceneObject('tree_000',5),mtscene.SceneObject('building_000',3),
             mtscene.SceneObject('ground',1),mtscene.SceneObject('tree_001',5)])])])


class TreeTest(unittest.TestCase):
    def test_shape_matches_crown_bounds_and_captured_top(self):
        g=fixture()[0].groups[0];src=[g.meshes[0]];original=trees.points(src)
        for shape in ('fitted','round','conifer'):
            new,report=trees.build(src,0,0,shape)
            p=trees.points(new)
            np.testing.assert_allclose(p[:,:2].min(0),original[:,:2].min(0),atol=1e-5)
            np.testing.assert_allclose(p[:,:2].max(0),original[:,:2].max(0),atol=1e-5)
            self.assertAlmostEqual(p[:,2].max(),original[:,2].max(),places=5)
            self.assertAlmostEqual(p[:,2].min(),original[:,2].min(),places=5)
            self.assertTrue(np.isfinite(p).all())
            self.assertLess(report['triangles'],500)
            for m in new:
                tri=m.vertices[m.draw_calls[0][1]].reshape(-1,3,3)
                self.assertTrue((np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1)>1e-8).all())

    def test_ground_inference_adds_trunk_without_moving_crown(self):
        g=fixture()[0].groups[0]
        new,report=trees.build([g.meshes[0]],0,ground=trees.GroundSamples(g))
        self.assertTrue(report['ground_inferred'])
        self.assertAlmostEqual(trees.points(new)[:,2].min(),0)
        self.assertAlmostEqual(trees.points(new)[:,2].max(),15)
        self.assertEqual(report['height_m'],15)

    def test_selected_only_preserves_originals_neighbors_and_texture_indices(self):
        scene=fixture();g=scene[0].groups[0];original=copy.deepcopy(g.meshes[0])
        unchanged=[fingerprint(m) for m in g.meshes[1:]];textures=copy.deepcopy(g.textures)
        self.assertEqual(trees.apply(scene,(0,0,0))[:2],(1,1))
        self.assertEqual([fingerprint(m) for m in g.meshes if m.object_id!=0],unchanged)
        archived=next(m for m in g.meshes if m.object_id==0 and m.model_variant==1)
        archived=copy.copy(archived);archived.model_variant=0
        self.assertEqual(fingerprint(archived),fingerprint(original))
        self.assertEqual(g.textures[:len(textures)],textures)
        self.assertEqual(len([m for m in g.meshes if m.object_id==0 and m.model_variant==2]),2)

    def test_reapply_replaces_instead_of_stacking_and_reuses_textures(self):
        scene=fixture();g=scene[0].groups[0]
        trees.apply(scene);before=[fingerprint(m) for m in g.meshes];textures=len(g.textures)
        trees.apply(scene)
        self.assertEqual(before,[fingerprint(m) for m in g.meshes])
        self.assertEqual(textures,len(g.textures))
        self.assertEqual(trees.apply(scene,use_ground=False)[0],2)

    def test_scene_roundtrip_preserves_variants_georeference_and_route_payload(self):
        scene=fixture();trees.apply(scene)
        route=dict(polyline=[(-122.4,37.7),(-122.401,37.701)],from_name='a',to_name='b',length_m=140)
        with tempfile.TemporaryDirectory() as d:
            a,b=Path(d)/'first.mtscene',Path(d)/'second.mtscene'
            mtscene.save(a,scene,route);loaded=mtscene.load(a)
            trees.apply(loaded);mtscene.save(b,loaded)
            after=mtscene.load(b);info={};mtscene.load(b,info)
            self.assertEqual(info['polyline'],route['polyline'])
            self.assertEqual(after[0].reference_pos,scene[0].reference_pos)
            self.assertEqual({m.model_variant for m in after[0].groups[0].meshes if m.object_id==0},{1,2})

    def test_invalid_target_and_degenerate_tree_leave_scene_unchanged(self):
        scene=fixture();g=scene[0].groups[0]
        with self.assertRaises(ValueError):trees.apply(scene,(0,0,1))
        g.meshes[0].vertices[:,2]=0
        before=[fingerprint(m) for m in g.meshes]
        done,total,records=trees.apply(scene,(0,0,0))
        self.assertEqual((done,total),(0,1));self.assertEqual(records[0]['status'],'unchanged')
        self.assertEqual(before,[fingerprint(m) for m in g.meshes])

    def test_cli_writes_paired_scene_and_report_and_rejects_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            src,out=Path(d)/'source.mtscene',Path(d)/'trees.mtscene'
            mtscene.save(src,fixture());before=src.read_bytes()
            with patch('sys.argv',['trees',str(src),str(out),'--target','0:0:0']),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(trees.main(),0)
            self.assertTrue(out.with_suffix('.trees.json').exists())
            self.assertEqual(before,src.read_bytes())
            with patch('sys.argv',['trees',str(src),str(src)]),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):trees.main()
            self.assertEqual(before,src.read_bytes())


if __name__=='__main__':unittest.main()

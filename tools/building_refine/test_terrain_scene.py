import copy,hashlib,json,tempfile,unittest
from pathlib import Path
import numpy as np
import mtscene,terrain_scene
from test_pcg import fixture,fingerprint

class TerrainSceneTest(unittest.TestCase):
 def test_buried_capture_layer_does_not_pull_streets_into_crater(self):
  yy,xx=np.mgrid[:240,:240];ground=(10+xx*.025+yy*.01).astype(np.float32)
  surface=ground.copy();surface[65:175,65:175]-=27
  height,trusted,_=terrain_scene.bare_ground(surface,np.ones(surface.shape,bool),1.)
  self.assertFalse(trusted[80:160,80:160].any())
  self.assertLess(np.max(abs(height[80:160,80:160]-ground[80:160,80:160])),.1)
  self.assertTrue(trusted[30,30])
 def test_terrain_only_and_future_placements_preserve_coordinates(self):
  batches=fixture();g=batches[0].groups[0];before=[fingerprint(m) for m in g.meshes]
  result,data,color,placements,stats=terrain_scene.rebuild_group(g,1.,2,lambda s:None)
  self.assertTrue(result.meshes)
  self.assertTrue(all(o.cls in (1,2,6,7) for o in result.objects))
  self.assertEqual(len(placements),2)
  self.assertEqual(placements[0]['world_anchor'][:2],[6.,5.])
  # This fixture includes observed ground underneath the building.
  self.assertFalse(placements[0]['terrain_inferred'])
  self.assertAlmostEqual(placements[0]['terrain_z'],-.01,places=4)
  self.assertLess(np.ptp(data['height'][data['cover']]),.001)
  self.assertEqual(before,[fingerprint(m) for m in g.meshes])
 def test_originals_win_over_generated_variants(self):
  g=fixture()[0].groups[0];original=copy.copy(g.meshes[0]);original.model_variant=1
  generated=copy.copy(original);generated.model_variant=2;generated.translation=np.array([0.,0.,100.])
  g.meshes=[original,generated]+g.meshes[1:]
  chosen=terrain_scene.original_group(g)
  self.assertIn(original,chosen.meshes)
  self.assertFalse(any(m is generated for m in chosen.meshes))
 def test_export_is_separate_georeferenced_and_refuses_overwrite(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);source=root/'source.mtscene';batches=fixture();batches[0].groups=batches[0].groups[:1];mtscene.save(source,batches)
   before=hashlib.sha256(source.read_bytes()).hexdigest()
   manifest=terrain_scene.export_scene(source,root/'terrain',1.,2)
   result=mtscene.load(root/'terrain/terrain.mtscene')
   self.assertEqual(result[0].reference_pos,batches[0].reference_pos)
   self.assertTrue(result[0].is_georeferenced)
   self.assertEqual(manifest['source_sha256'],before)
   self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
   with self.assertRaises(ValueError):terrain_scene.export_scene(source,root/'terrain')
 def test_no_ground_does_not_make_a_roof_into_terrain(self):
  g=fixture()[0].groups[0];g.meshes=g.meshes[:2]
  with self.assertRaises(ValueError):terrain_scene.rebuild_group(g,1.,2,lambda s:None)
 def test_tree_canopy_never_becomes_ground_height(self):
  g=fixture()[0].groups[0]
  p=np.array([[60,0,20],[70,0,20],[70,10,20],[60,0,20],[70,10,20],[60,10,20]],np.float32)
  g.objects.append(mtscene.SceneObject('forest_canopy',5))
  g.meshes.append(mtscene.Mesh(0xffffffff,3,np.zeros(3),p,None,None,[(4,np.arange(6,dtype=np.uint32))]))
  _,data,_,placements,_=terrain_scene.rebuild_group(g,1.,2,lambda s:None)
  self.assertTrue(data['tree_cover'].any())
  self.assertTrue(np.allclose(data['height'][data['tree_cover']],-.01,atol=.001))
  tree=next(p for p in placements if p['kind']=='tree')
  self.assertAlmostEqual(tree['terrain_z'],-.01,places=4)
 def test_mislabelled_canopy_filtered_and_hidden_ground_follows_slope(self):
  yy,xx=np.mgrid[:100,:100];ground=(xx*.05+yy*.02).astype(np.float32)
  surface=ground.copy();surface[35:65,35:65]+=18
  height,trusted,distance=terrain_scene.bare_ground(surface,np.ones(surface.shape,bool),1.)
  self.assertFalse(trusted[45:55,45:55].any())
  self.assertLess(np.max(abs(height[40:60,40:60]-ground[40:60,40:60])),.1)
  self.assertGreater(distance[50,50],10)
 def test_ground_labelled_duplicate_of_tree_is_not_trusted(self):
  g=fixture()[0].groups[0]
  p=np.array([[60,0,20],[70,0,20],[70,10,20],[60,0,20],[70,10,20],[60,10,20]],np.float32)
  g.objects.append(mtscene.SceneObject('tree',5))
  tree=mtscene.Mesh(0xffffffff,3,np.zeros(3),p,None,None,[(4,np.arange(6,dtype=np.uint32))])
  duplicate=copy.copy(tree);duplicate.object_id=2
  g.meshes.extend([tree,duplicate])
  _,data,_,_,_=terrain_scene.rebuild_group(g,1.,2,lambda s:None)
  self.assertTrue(data['inferred'][data['tree_cover']].all())
  self.assertTrue(np.allclose(data['height'][data['tree_cover']],-.01,atol=.001))

if __name__=='__main__':unittest.main()

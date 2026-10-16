import copy,json,tempfile,unittest,hashlib
from pathlib import Path
import numpy as np
import mtscene
from test_pcg import fixture,fingerprint
from batch_buildings import proxy_model
from compose_city import compose

class CityBuildingsTest(unittest.TestCase):
 def test_procedural_fallback_has_finite_geometry_at_original_location(self):
  g=fixture()[0].groups[0];before=fingerprint(g.meshes[0]);m=proxy_model([g.meshes[0]],0)
  self.assertEqual(before,fingerprint(g.meshes[0]));self.assertEqual(m.model_variant,2)
  world=m.vertices+m.translation;self.assertTrue(np.isfinite(world).all())
  self.assertAlmostEqual(world[:,2].min(),0,places=4);self.assertAlmostEqual(world[:,2].max(),12,places=4)
 def test_composition_replaces_existing_building_keeps_terrain(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp);source=root/'original.mtscene';scene=mtscene.Scene(fixture());scene[0].groups=scene[0].groups[:1]
   mtscene.save(source,scene);before=hashlib.sha256(source.read_bytes()).hexdigest()
   assets=root/'assets';assets.mkdir();records=[]
   for oid in (0,1):
    mesh=proxy_model([scene[0].groups[0].meshes[oid]],0)
    asset=mtscene.Scene([mtscene.Batch(False,True,True,scene[0].reference_pos,[mtscene.Group([], [mesh],[scene[0].groups[0].objects[oid]])])])
    mtscene.save(assets/f'{oid}.mtscene',asset)
    records.append(dict(batch=0,group=0,object=oid,name=scene[0].groups[0].objects[oid].name,asset=f'{oid}.mtscene',method='procedural_fragment'))
   (assets/'manifest.json').write_text(json.dumps(dict(source=str(source),source_sha256=before,buildings=records)))
   report=compose(source,assets,root/'out');result=mtscene.load(root/'out/city_pcg.mtscene')[0].groups[0]
   self.assertEqual(report['building_count'],2);self.assertEqual(sum(o.cls==3 for o in result.objects),2)
   np.testing.assert_array_equal(result.meshes[0].vertices,scene[0].groups[0].meshes[2].vertices)
   self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),before)
   with self.assertRaises(ValueError):compose(source,assets,root/'out')

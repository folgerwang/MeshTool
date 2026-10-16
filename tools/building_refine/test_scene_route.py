"""Version 6 route metadata remains intact across Python scene processing."""
import struct,tempfile,unittest
from pathlib import Path
import mtscene
from test_pcg import fixture

class SceneRouteTest(unittest.TestCase):
 def test_explicit_route_and_lod_survive_implicit_save(self):
  scene=mtscene.Scene(fixture());scene[0].groups[0].meshes[0].lod_size=24.5
  route=dict(polyline=[(-122.,37.),(-122.001,37.001)],from_name='Start',to_name='End',length_m=150.)
  with tempfile.TemporaryDirectory() as tmp:
   a,b=Path(tmp)/'a.mtscene',Path(tmp)/'b.mtscene'
   mtscene.save(a,scene,route=route);parsed={};loaded=mtscene.load(a,route=parsed)
   self.assertEqual(parsed['polyline'],route['polyline'])
   self.assertEqual(loaded[0].groups[0].meshes[0].lod_size,24.5)
   mtscene.save(b,loaded);self.assertEqual(a.read_bytes(),b.read_bytes())

 def test_route_payload_roundtrips_and_truncation_is_rejected(self):
  payload=struct.pack('<I4d',2,-122.,37.,-122.001,37.001)
  for name in ('Start','Destination'):
   b=name.encode();payload+=struct.pack('<I',len(b))+b
  payload+=struct.pack('<5d',-122.,37.,-122.001,37.001,150.)
  scene=mtscene.Scene(fixture(),route_data=payload)
  with tempfile.TemporaryDirectory() as tmp:
   a,b=Path(tmp)/'a.mtscene',Path(tmp)/'b.mtscene'
   mtscene.save(a,scene);loaded=mtscene.load(a)
   self.assertEqual(loaded.route_data,payload)
   mtscene.save(b,loaded);self.assertEqual(a.read_bytes(),b.read_bytes())
   b.write_bytes(a.read_bytes()[:-1])
   with self.assertRaises(ValueError):mtscene.load(b)

if __name__=='__main__':unittest.main()

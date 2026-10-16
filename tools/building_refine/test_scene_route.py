"""Version 6 route metadata remains intact across Python scene processing."""
import struct,tempfile,unittest
from pathlib import Path
import mtscene
from test_pcg import fixture

class SceneRouteTest(unittest.TestCase):
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

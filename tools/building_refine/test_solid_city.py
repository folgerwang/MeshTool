import unittest
from shapely.geometry import Polygon
from solid_city import shell,audit_shell,roof_zones

class SolidCityTest(unittest.TestCase):
 def test_large_building_retains_distinct_roof_levels(self):
  low=Polygon([(0,0),(40,0),(40,20),(0,20)])
  high=Polygon([(40,0),(80,0),(80,20),(40,20)])
  parts=roof_zones(low.union(high),shell(low,0,20,0)+shell(high,0,60,0),0,60)
  self.assertGreater(max(z for _,z in parts)-min(z for _,z in parts),30)
  for poly,z in parts:self.assertFalse(any(audit_shell(shell(poly,0,z,0)).values()))
 def test_concave_shell_has_no_holes_or_nonmanifold_edges(self):
  polygon=Polygon([(0,0),(12,0),(12,4),(5,4),(5,10),(0,10)])
  self.assertEqual(audit_shell(shell(polygon,3,25,0)),dict(boundary_edges=0,nonmanifold_edges=0,degenerate_triangles=0))
 def test_courtyard_has_closed_inner_walls_and_roof_ring(self):
  polygon=Polygon([(0,0),(20,0),(20,20),(0,20)],[[(5,5),(5,15),(15,15),(15,5)]])
  self.assertFalse(any(audit_shell(shell(polygon,10,40,0)).values()))
 def test_audit_detects_missing_roof(self):
  meshes=shell(Polygon([(0,0),(10,0),(10,10),(0,10)]),0,12,0)
  self.assertGreater(audit_shell(meshes[:1])['boundary_edges'],0)

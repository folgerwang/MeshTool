import unittest
import numpy as np
import mtscene
from road_scene import network,rebuild,road_surface,continuous_red_lanes,repair_road_mask,restore_ground
from test_pcg import fingerprint,fixture

class RoadsTest(unittest.TestCase):
 def test_replace_roads_restores_saved_terrain_and_preserves_building_assets(self):
  import copy
  group=fixture()[0].groups[0];clean=copy.deepcopy(group)
  clean.meshes=clean.meshes[2:]
  before=list(map(fingerprint,group.meshes[:2]))
  old_signal=copy.deepcopy(group.meshes[0]);old_signal.object_id=len(group.objects)
  group.objects.append(mtscene.SceneObject('PCG inferred traffic signals',0));group.meshes.append(old_signal)
  restore_ground(group,clean)
  self.assertEqual(before,list(map(fingerprint,group.meshes[:2])))
  self.assertEqual(len(group.meshes),3)
  np.testing.assert_array_equal(group.meshes[-1].vertices,clean.meshes[0].vertices)

 def test_collinear_red_fragments_become_one_constant_width_lane(self):
  mask=np.zeros((140,100),bool);mask[10:45,40:46]=True;mask[65:120,40:46]=True
  road=np.ones_like(mask);paint,lanes=continuous_red_lanes(mask,road,.5)
  self.assertEqual(len(lanes),1)
  self.assertTrue((paint[90:130,84:88]>0).all())
  a,b=np.array(lanes[0]['start_pixel']),np.array(lanes[0]['end_pixel'])
  self.assertAlmostEqual(a[0],b[0],places=4)
  self.assertGreater(lanes[0]['length_m'],50)
 def test_separate_parallel_lanes_and_real_road_gap_are_not_joined(self):
  mask=np.zeros((140,100),bool);mask[10:100,20:26]=True;mask[10:100,40:46]=True
  _,lanes=continuous_red_lanes(mask,np.ones_like(mask),.5)
  self.assertEqual(len(lanes),2)
  mask[:]=False;mask[10:45,40:46]=True;mask[65:120,40:46]=True
  road=np.ones_like(mask);road[48:62]=False
  _,lanes=continuous_red_lanes(mask,road,.5)
  self.assertEqual(len(lanes),2)
 def test_road_repair_fills_short_break_but_preserves_city_block(self):
  road=np.zeros((200,200),bool);road[:,80:120]=True;road[98:102,80:120]=False
  data=dict(res=.5,cover=np.ones_like(road))
  repaired=repair_road_mask(road,data)
  self.assertTrue(repaired[100,100]);self.assertFalse(repaired[100,20])
 def data(self):
  n=220;y,x=np.mgrid[:n,:n];road=(abs(x-110)<12)|(abs(y-110)<12)
  return dict(res=np.array(.5),min_x=np.array(0.),max_y=np.array(110.),height=(4+x*.01+y*.005).astype(np.float32),cover=np.ones((n,n),bool),source_class=np.where(road,2,1))
 def test_cross_has_one_junction_and_no_built_up_asphalt(self):
  data=self.data();road,sk,j=network(data)
  self.assertEqual(len(j),1);self.assertEqual(len(j[0]['approaches']),4)
  self.assertFalse(road[data['source_class']==3].any())
 def test_straight_road_has_no_traffic_signals(self):
  data=self.data();data['source_class'][:]=1;data['source_class'][:,98:122]=2
  self.assertEqual(network(data)[2],[])
 def test_added_geometry_preserves_buildings_and_follows_elevation(self):
  group=fixture()[0].groups[0];before=list(map(fingerprint,group.meshes[:2]));count=2
  data=self.data();report,road=rebuild(group,data)
  self.assertEqual(before,list(map(fingerprint,group.meshes[:2])))
  self.assertGreater(report['signal_count'],0)
  for x,y,z in report['signal_positions']:
   ix=round(x/.5);iy=round((110-y)/.5)
   self.assertFalse(road[iy,ix]);self.assertAlmostEqual(z,4+(x/.5)*.01+((110-y)/.5)*.005,places=4)
  for mesh in group.meshes[count:]:
   self.assertTrue(np.isfinite(mesh.vertices).all())
   if mesh.tex_index!=0xffffffff:self.assertLess(mesh.tex_index,len(group.textures))
   self.assertLess(int(mesh.draw_calls[0][1].max()),len(mesh.vertices))
 def test_road_partition_has_no_overlap_and_exact_shared_seam(self):
  y,x=np.mgrid[:5,:5];vertices=np.c_[x.ravel(),y.ravel(),(x*.1+y*.05).ravel()].astype(np.float32)
  triangles=[]
  for row in range(4):
   for col in range(4):
    a=row*5+col;triangles.extend([[a,a+1,a+6],[a,a+6,a+5]])
  indices=np.array(triangles,np.uint32).ravel()
  mesh=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,indices)])
  group=mtscene.Group([], [mesh],[mtscene.SceneObject('terrain',1)])
  data=dict(res=1.,min_x=0.,max_y=4.)
  road=np.zeros((5,5),bool);road[:,2:]=True
  road_surface(group,data,road)
  ground=group.meshes[0];asphalt=group.meshes[1]
  a={tuple(t) for _,idx in ground.draw_calls for t in idx.reshape(-1,3)}
  b={tuple(t) for _,idx in asphalt.draw_calls for t in idx.reshape(-1,3)}
  self.assertFalse(a&b);self.assertEqual(a|b,{tuple(t) for t in triangles})
  shared=np.intersect1d(ground.draw_calls[0][1],asphalt.draw_calls[0][1])
  self.assertGreater(len(shared),0)
  np.testing.assert_array_equal(ground.vertices[shared],asphalt.vertices[shared])
  self.assertEqual(len(group.meshes),2) # paint does not add a coplanar layer

import unittest
import numpy as np
from road_network import corridors,recover_surface,building_mask

class RoadNetworkTest(unittest.TestCase):
 def test_appearance_and_length_support_reject_forecourt_and_sidewalk(self):
  seed=np.zeros((160,100),bool);seed[:,45:55]=True
  classes=np.ones(seed.shape,int);classes[:,35:65]=2;classes[65:85,65:75]=2
  image=np.full(seed.shape+(3,),75,np.uint8);image[:,:40]=205
  data=dict(res=.5,cover=np.ones_like(seed),height=np.zeros(seed.shape),captured_height=np.zeros(seed.shape),source_class=classes)
  grown,_=recover_surface(seed,data,surface=image)
  self.assertTrue(grown[40,60]);self.assertFalse(grown[75,70]);self.assertFalse(grown[40,36])

 def test_capture_recovers_wider_street_and_flared_junction(self):
  y,x=np.mgrid[:100,:100];seed=abs(x-50)<5
  observed=(abs(x-50)<12)|((abs(y-50)<12)&(abs(x-50)<20))
  data=dict(res=.5,cover=np.ones_like(seed),height=np.zeros(seed.shape),captured_height=np.zeros(seed.shape),source_class=np.where(observed,2,1))
  grown,report=recover_surface(seed,data)
  self.assertTrue(grown[20,40]);self.assertTrue(grown[50,33])
  self.assertFalse(grown[20,30]);self.assertGreater(report['recovered_road_m2'],0)
 def test_rejects_road_labelled_roof_and_distant_paved_plot(self):
  seed=np.zeros((100,150),bool);seed[:,45:55]=True
  classes=np.ones(seed.shape,int);classes[:,35:65]=2;classes[20:60,100:140]=2
  heights=np.zeros(seed.shape);heights[30:60,55:65]=20
  data=dict(res=.5,cover=np.ones_like(seed),height=np.zeros(seed.shape),captured_height=heights,source_class=classes)
  excluded=np.zeros_like(seed);excluded[70:85,35:45]=True
  grown,_=recover_surface(seed,data,excluded)
  self.assertFalse(grown[40,60]);self.assertFalse(grown[40,110]);self.assertFalse(grown[75,40])
  self.assertTrue(grown[15,40])
 def test_projected_building_mask_preserves_courtyard(self):
  from shapely.geometry import Polygon
  from solid_city import shell
  import mtscene
  poly=Polygon([(2,2),(18,2),(18,18),(2,18)],holes=[[(6,6),(14,6),(14,14),(6,14)]])
  group=mtscene.Group([],shell(poly,0,10,0),[mtscene.SceneObject('building',3)])
  mask=building_mask(group,dict(cover=np.ones((21,21),bool),res=1.,min_x=0.,max_y=20.))
  self.assertTrue(mask[3,3]);self.assertFalse(mask[10,10])

 def fixture(self):
  data=dict(cover=np.ones((100,100),bool),res=.5,min_x=0.,max_y=50.)
  profile=dict(registration=dict(shape=[100,100],res=.5,min_x=0.,max_y=50.),evidence='test',map_url='test',roads=[
   dict(id='main',width_m=6,points=[[0,50],[99,50]]),dict(id='side',width_m=6,points=[[50,0],[50,46]])])
  return profile,data
 def test_t_junction_snaps_to_segment_and_connects(self):
  profile,data=self.fixture();mask,nodes,report=corridors(profile,data)
  self.assertEqual(report['raster_components'],1)
  self.assertEqual(len(nodes),1);self.assertEqual(len(nodes[0]['approaches']),3)
  self.assertTrue(mask[50,50]);self.assertFalse(mask[90,90])
 def test_registration_mismatch_rejected(self):
  profile,data=self.fixture();data['min_x']=1
  with self.assertRaises(ValueError):corridors(profile,data)
 def test_segmentation_holes_do_not_break_map_road(self):
  profile,data=self.fixture();data['source_class']=np.zeros((100,100),np.int32)
  mask,nodes,report=corridors(profile,data)
  self.assertTrue(mask[50].all());self.assertEqual(report['raster_components'],1)

"""Row staggering is measured from capture and changes actual window geometry."""
import unittest
import numpy as np
import facade,facade_pattern,mtscene

class RowPatternTest(unittest.TestCase):
 def image(self,offsets):
  res=.1;floor=3.;bay=2.;y,x=np.mgrid[:360,:200]
  row=(y*res//floor).astype(int);shift=np.array(offsets)[row%len(offsets)]*bay
  bright=((x*res-shift)%bay<.25)|((y*res)%floor<.5)
  rgb=np.repeat(np.where(bright,190,45).astype(np.uint8)[:,:,None],3,axis=2)
  return rgb[::-1],np.ones(bright.shape,bool),res,bay,floor
 def test_detects_half_bay_alternation(self):
  found=facade_pattern.row_pattern(*self.image([0,.5]))
  self.assertIsNotNone(found);self.assertEqual(found['offsets'],[0.,.5])
 def test_detects_three_row_cycle(self):
  found=facade_pattern.row_pattern(*self.image([0,0,.5]))
  self.assertIsNotNone(found);self.assertEqual(found['offsets'],[0.,0.,.5])
 def test_detects_four_row_quarter_bay_cycle(self):
  found=facade_pattern.row_pattern(*self.image([0,.25,.5,.75]))
  self.assertIsNotNone(found);self.assertEqual(found['offsets'],[0.,.25,.5,.75])
 def test_spacing_detection_does_not_alias_staggered_rows(self):
  image,mask,res,bay,floor=self.image([0,.25,.5,.75])
  found=facade_pattern.image_pattern(image,mask,res)
  self.assertAlmostEqual(found['bay']['spacing'],bay)
  rows=facade_pattern.row_pattern(image,mask,res,found['bay']['spacing'],found['floor']['spacing'],found['floor']['phase'])
  self.assertIsNotNone(rows);self.assertEqual(rows['offsets'],[0.,.25,.5,.75])
 def test_aligned_and_occluded_rows_do_not_invent_staggering(self):
  self.assertIsNone(facade_pattern.row_pattern(*self.image([0.])))
  image,mask,res,bay,floor=self.image([0,.5]);mask[:240]=False
  self.assertIsNone(facade_pattern.row_pattern(image,mask,res,bay,floor))
 def test_offset_changes_pane_centers_by_half_bay_and_keeps_boundary(self):
  vertices=np.array([[0,0,0],[16,0,0],[16,0,12],[0,0,0],[16,0,12],[0,0,12]],np.float32)
  source=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,np.arange(6,dtype=np.uint32))])
  pattern={'floor':{'spacing':3.,'anchor_z':0.},'bay':{'spacing':2.}}
  meshes,_,_=facade.build([source],0,0,parameters={'row_offsets':[0.,.5]},detected_pattern=pattern)
  panes=np.concatenate([m.vertices+m.translation for m in meshes if m.material==1]).reshape(-1,6,3)
  centers=panes.mean(1);centers=centers[(centers[:,0]>2)&(centers[:,0]<14)]
  # Level 0 is a storefront row whose entrance splits one pane around the door.
  for level in range(1,4):
   x=centers[(centers[:,2]>=level*3)&(centers[:,2]<(level+1)*3),0]
   self.assertGreater(len(x),3)
   distance=np.abs(((x-(1. if level%2==0 else 0.)+1)%2)-1)
   self.assertLess(distance.max(),1e-5)
  self.assertGreaterEqual(panes[:,:,0].min(),0);self.assertLessEqual(panes[:,:,0].max(),16)

if __name__=='__main__':unittest.main()

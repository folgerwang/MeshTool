import unittest,numpy as np
from validate_window_candidates import validate
class CandidateValidationTests(unittest.TestCase):
 def setUp(self):
  self.p={'bounds':[1,1,3,3]};self.d=dict(bounds=[1,1,3,3],pixel_rect=[1,1,3,3],kind='window',confidence=.9,shape='rectangular')
 def test_observed_window_accepted(self):
  d,s=validate(self.p,[self.d],np.ones((5,5),bool),1);self.assertEqual(s,'accepted');self.assertEqual(d['surface_mode'],'flush')
 def test_hallucinated_gap_rejected(self):
  mask=np.ones((5,5),bool);mask[1:3,1:3]=False
  self.assertIsNone(validate(self.p,[self.d],mask,1)[0])
 def test_mismatched_shape_rejected(self):
  d=dict(self.d,bounds=[4,1,5,3]);self.assertIsNone(validate(self.p,[d],np.ones((5,5),bool),1)[0])
 def test_no_detection_rejected(self):
  self.assertIsNone(validate(self.p,[],np.ones((5,5),bool),1)[0])
if __name__=='__main__':unittest.main()

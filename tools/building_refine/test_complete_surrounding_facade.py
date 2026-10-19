import unittest,numpy as np
from complete_surrounding_facade import repeat_bay
class SurroundingFacadeTests(unittest.TestCase):
 def test_preserves_observed_pixels_and_row_pattern(self):
  donor=np.zeros((20,8,3),np.uint8)
  for y in range(20):
   donor[y,:,0]=y*10;donor[y,:,1]=np.arange(8)*20
  target=np.tile(donor,(1,3,1));known=np.zeros((20,24),bool);known[:,0:2]=True;target[:,0,2]=91
  result,phase,error=repeat_bay(donor,target,known,0,8)
  np.testing.assert_array_equal(result[known],target[known]);np.testing.assert_array_equal(result[:,3,0],np.arange(20)*10)
  np.testing.assert_array_equal(result[:,3:8],result[:,11:16])
 def test_rejects_unregistered_floors(self):
  with self.assertRaises(ValueError):repeat_bay(np.zeros((3,8,3),np.uint8),np.zeros((4,8,3),np.uint8),np.ones((4,8),bool),0,4)
 def test_rejects_no_observed_alignment(self):
  with self.assertRaises(ValueError):repeat_bay(np.zeros((20,8,3),np.uint8),np.zeros((20,8,3),np.uint8),np.zeros((20,8),bool),0,4)
if __name__=='__main__':unittest.main()

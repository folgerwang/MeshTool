import unittest
import numpy as np
import captured_facade


class CapturedFacadeTest(unittest.TestCase):
    def test_base_extension_repeats_lowest_storey_only_below_capture(self):
        pitch=.1;h,w=200,40
        image=np.zeros((h,w,3),np.uint8);mask=np.zeros((h,w),bool)
        # Captured upper 120 rows: storey bands alternate every 18 rows.
        image[:120]=np.where((np.arange(120)//18%2==0)[:,None,None],200,60)
        mask[:120]=True
        mask[:,30:]=False;mask[:20,30:]=True        # a column too short to extend
        out,covered,filled=captured_facade.extend_base(image,mask,pitch,band_m=3.6)
        np.testing.assert_array_equal(out[:120,:30],image[:120,:30])
        self.assertTrue(covered[:,:30].all())
        np.testing.assert_array_equal(out[120:156,:30],out[84:120,:30])
        self.assertFalse(covered[25:,30:].any())
        self.assertEqual(filled,80*30)


if __name__=='__main__':unittest.main()

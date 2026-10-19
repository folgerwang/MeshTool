import unittest
import numpy as np
from ai_city_iteration import wall_gap_mask


class AiCityIterationTest(unittest.TestCase):
    def test_large_blank_center_is_not_extrapolated(self):
        covered=np.zeros((100,100),bool);covered[:,:20]=True
        mask=wall_gap_mask(covered,[],.1)
        self.assertTrue(mask[50,24]);self.assertFalse(mask[50,80])
        self.assertFalse(mask[covered].any())

    def test_openings_and_proposed_windows_are_not_painted_on_walls(self):
        covered=np.ones((100,100),bool);covered[35:65,35:65]=False
        mask=wall_gap_mask(covered,[dict(bounds=[3.,3.,7.,7.])],.1)
        self.assertFalse(mask.any())


if __name__=='__main__':unittest.main()

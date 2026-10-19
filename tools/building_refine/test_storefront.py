import unittest
import numpy as np
from storefront import detect,measure_members

class StorefrontTest(unittest.TestCase):
    def image(self):
        im=np.full((120,150,3),200,np.uint8);im[55:105,20:120]=(75,95,110)
        for x in (40,65,90):im[55:105,x:x+2]=(25,30,35)
        for y in (70,90):im[y:y+2,20:120]=(25,30,35)
        return im
    def test_large_ground_grid_detected_without_repeated_upper_rows(self):
        found=detect(self.image(),.08)
        self.assertEqual(len(found),1)
        self.assertEqual(found[0]['kind'],'window')
        self.assertEqual(found[0]['recovery'],'captured_storefront')
    def test_upper_floor_does_not_become_storefront(self):
        self.assertFalse(detect(self.image(),.08,8.))
    def test_plain_garage_panel_not_glazing(self):
        im=self.image();im[55:105,20:120]=(75,95,110)
        self.assertFalse(detect(im,.08))
    def test_dark_mullions_are_measured(self):
        members=measure_members(self.image()[55:105,20:120],8.,4.)
        self.assertEqual({m['axis'] for m in members},{'x','y'})

import unittest
import numpy as np
from curtain_wall import detect


class CurtainWallTest(unittest.TestCase):
    def test_dark_metal_and_narrow_strips_survive_material_cleanup(self):
        from building_rules import captured_wall_material
        im=np.full((80,80,3),(25,32,40),np.uint8)
        openings=[dict(recovery='captured_curtain_bay',pixel_rect=[a,10,b,70],bounds=[a*.1,1,b*.1,7]) for a,b in [(10,30),(32,52)]]
        finish,mask=captured_wall_material(im,openings,.1)
        self.assertFalse(mask[:,30:32].any())
        np.testing.assert_array_equal(finish[:,30:32],im[:,30:32])
        np.testing.assert_array_equal(finish[:5],im[:5])

    def capture(self):
        im=np.full((600,400,3),(40,55,70),np.uint8)
        for y in range(10,590,60):im[y:y+4]=220
        for x in range(10,390,20):im[:,x:x+2]=135
        return im

    def test_recovers_glass_between_observed_bands(self):
        items=detect(self.capture(),.05)
        self.assertGreater(len(items),100)
        self.assertTrue(all(i['kind']=='window' for i in items))
        self.assertTrue(all(i['bounds'][3]-i['bounds'][1]<3.2 for i in items))

    def test_no_bays_on_blank_wall_or_light_masonry(self):
        self.assertEqual(detect(np.full((600,400,3),150,np.uint8),.05),[])
        im=self.capture();im[im.mean(2)<100]=210
        self.assertEqual(detect(im,.05),[])

    def test_occluder_without_local_floor_bands_does_not_get_windows(self):
        im=self.capture();im[:,50:150]=(35,45,55)
        items=detect(im,.05)
        self.assertGreater(len(items),50)
        self.assertFalse(any(60<(o['pixel_rect'][0]+o['pixel_rect'][2])/2<140 for o in items))

    def test_unresolved_mullions_do_not_create_arbitrary_columns(self):
        im=np.full((600,400,3),(40,55,70),np.uint8)
        for y in range(10,590,60):im[y:y+4]=220
        items=detect(im,.05)
        self.assertGreater(len(items),5)
        self.assertTrue(all(i['recovery']=='captured_glazed_band' for i in items))

    def test_openings_preserve_locally_curved_floor_bands(self):
        im=np.full((600,400,3),(40,55,70),np.uint8)
        for x in range(400):
            shift=round(6*((x-200)/200)**2)
            for y in range(10,590,60):im[y+shift:y+shift+3,x]=220
        for x in range(10,390,20):im[:,x:x+2]=135
        items=detect(im,.05)
        self.assertGreater(len(items),100)
        for item in items:
            a,b,c,d=item['pixel_rect']
            self.assertLess(np.mean(im[b:d,a:c].mean(2)>200),.04)

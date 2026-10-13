import unittest
import numpy as np
import facade_pattern as pattern

class PatternTest(unittest.TestCase):
    def test_period_and_phase_from_repeated_bands(self):
        x=np.arange(800)*.1
        y=100+25*np.cos(2*np.pi*(x-.7)/3.2)+.05*x
        found=pattern.period(y,np.ones(len(y),bool),.1,(2.6,5.2))
        self.assertIsNotNone(found)
        self.assertAlmostEqual(found['spacing'],3.2,places=5)
        self.assertGreater(found['confidence'],.9)
        self.assertLess(abs((found['phase']-.7+1.6)%3.2-1.6),.15)

    def test_flat_noisy_and_insufficient_capture_have_no_pattern(self):
        rng=np.random.default_rng(42)
        for profile in (np.full(600,120.),rng.normal(120,15,600),np.linspace(20,220,600)):
            self.assertIsNone(pattern.period(profile,np.ones(600,bool),.1,(2.6,5.2)))
        x=np.arange(600)*.1
        self.assertIsNone(pattern.period(100+20*np.cos(x*2*np.pi/3.2),np.arange(600)<60,.1,(2.6,5.2)))

    def test_panel_color_ignores_dark_glazing_and_clipped_highlights(self):
        image=np.full((100,100,3),[15,35,65],np.uint8)
        image[40:95]=[160,145,125]
        image[95:]=255
        found=pattern.panel_color(image,np.ones((100,100),bool))
        np.testing.assert_allclose(found['rgb'],np.array([160,145,125])*1.12,atol=1)
        self.assertIsNone(pattern.panel_color(image,np.zeros((100,100),bool)))

    def test_two_dimensional_grid(self):
        y,x=np.mgrid[:640,:240]
        gray=120+20*np.cos(2*np.pi*x/18)+25*np.cos(2*np.pi*y/36)
        image=np.repeat(gray[:,:,None],3,axis=2).astype(np.uint8)
        result=pattern.image_pattern(image,np.ones(gray.shape,bool),.1)
        self.assertAlmostEqual(result['bay']['spacing'],1.8,places=5)
        self.assertAlmostEqual(result['floor']['spacing'],3.6,places=5)

    def test_column_classes_follow_capture_composition(self):
        # Dark blue glazing, a bright cladding zone, one lone bright pier, and
        # an uncovered column that follows its neighbour.
        res,bay=.1,1.6
        image=np.zeros((300,16*16,3),np.uint8);image[:]=[30,45,70]
        for col in (5,6,7,8,12):image[:,col*16:(col+1)*16]=[150,148,140]
        mask=np.ones(image.shape[:2],bool);mask[:,3*16:4*16]=False
        found=pattern.column_classes(image,mask,res,bay,140.,16)
        c=found['columns']
        self.assertEqual(c[5:9],['clad']*4)
        self.assertEqual(c[12],'pier')
        self.assertEqual(c[3],'glazed')
        self.assertEqual({c[i] for i in (0,1,2,4,9,10,11,13,14,15)},{'glazed'})
        self.assertEqual(found['sampled'],15)
        # A uniformly bright patch is cladding; a uniformly dark blue one is glazing.
        bright=np.full((300,160,3),[150,148,140],np.uint8)
        self.assertEqual(set(pattern.column_classes(bright,np.ones((300,160),bool),res,bay,140.,10)['columns']),{'clad'})
        self.assertEqual(set(pattern.column_classes(image[:,:160].copy()*0+np.array([30,45,70],np.uint8),np.ones((300,160),bool),res,bay,140.,10)['columns']),{'glazed'})
        # Without coverage every column defaults to cladding.
        self.assertEqual(pattern.column_classes(image,np.zeros(mask.shape,bool),res,bay,140.,16)['sampled'],0)

    def test_reference_luminance_undoes_albedo_compensation(self):
        self.assertAlmostEqual(pattern.reference_luminance({'appearance':{'rgb':(112,112,112)}}),100.,places=5)
        self.assertEqual(pattern.reference_luminance({}),120.)

if __name__=='__main__':unittest.main()

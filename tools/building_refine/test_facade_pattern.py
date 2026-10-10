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

if __name__=='__main__':unittest.main()

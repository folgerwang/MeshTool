import unittest
import numpy as np
from facade_inpaint import repair


class FakeModel:
    def fill(self,image,mask,log=print):
        return np.full_like(image,99)


class FacadeInpaintTest(unittest.TestCase):
    def test_ml_cannot_overwrite_observed_pixels(self):
        rng=np.random.default_rng(7);image=rng.integers(20,200,(80,100,3),dtype=np.uint8)
        known=np.ones((80,100),bool);known[25:45,35:60]=False
        result,inferred,report=repair(image,known,.1,FakeModel())
        np.testing.assert_array_equal(result[known],image[known])
        self.assertTrue(inferred[30,40]);self.assertGreater(report['ml_pixels'],0)

    def test_no_reference_does_not_hallucinate_entire_facade(self):
        image=np.zeros((80,100,3),np.uint8);known=np.zeros((80,100),bool)
        result,inferred,report=repair(image,known,.1,FakeModel())
        self.assertFalse(inferred.any());self.assertEqual(report['ml_pixels'],0)

    def test_fully_observed_facade_is_unchanged(self):
        image=np.full((40,60,3),150,np.uint8)
        result,inferred,report=repair(image,np.ones((40,60),bool),.1,FakeModel())
        np.testing.assert_array_equal(result,image);self.assertFalse(inferred.any())

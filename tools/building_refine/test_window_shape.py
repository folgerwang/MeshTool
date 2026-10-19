import unittest
import numpy as np
from window_shape import refine


class WindowShapeTest(unittest.TestCase):
    def fixtures(self):
        image=np.full((120,200,3),190,np.uint8)
        items=[]
        for x in [1,5,9,13]:
            image[30:90,int(x*10):int((x+3)*10)]=35
            items.append(dict(bounds=[x,3,x+3,9],kind='window',shape='rectangular'))
        items[0]['shape']='arched'
        return image,items

    def test_rectangular_lintel_repairs_false_arch_and_converges(self):
        image,items=self.fixtures();result,changes=refine(image,items,.1)
        self.assertEqual(len(changes),1);self.assertEqual(result[0]['shape'],'rectangular')
        self.assertEqual(result[0]['bounds'],items[0]['bounds'])
        self.assertEqual(refine(image,result,.1)[1],[])

    def test_real_arched_crown_is_retained(self):
        image,items=self.fixtures()
        image[25:55,10:40]=190
        for x in range(10,40):
            y=45-int(np.sqrt(max(0,15**2-(x-25)**2)))
            image[y:90,x]=35
        result,changes=refine(image,items,.1)
        self.assertEqual(changes,[]);self.assertEqual(result[0]['shape'],'arched')

    def test_isolated_arch_not_changed_by_lintel_alone(self):
        image,items=self.fixtures();self.assertEqual(refine(image,items[:1],.1)[1],[])


if __name__=='__main__':unittest.main()

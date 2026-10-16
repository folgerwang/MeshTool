import json,unittest
import numpy as np
from road_finish import smooth_boundary,CONFIG

class RoadFinishTest(unittest.TestCase):
    def test_shared_zigzag_seam_smooths_without_folding(self):
        points=np.array([[x,y,0.] for y in range(12) for x in range(8)],float)
        tris=[];labels=[]
        for y in range(11):
            for x in range(7):
                a=y*8+x
                tris.extend([[a,a+1,a+9],[a,a+9,a+8]])
                labels.extend([x<3+(y%2)]*2)
        tris=np.array(tris);cfg=json.loads(CONFIG.read_text())
        out,report=smooth_boundary(points,tris,np.array(labels),cfg)
        self.assertGreater(report['moved_vertices'],5)
        self.assertLessEqual(report['max_shift_m'],cfg['max_edge_shift_m']+1e-8)
        p=out[tris];a=p[:,1]-p[:,0];b=p[:,2]-p[:,0]
        self.assertTrue(np.all(np.cross(a,b)[:,2]>0))
        np.testing.assert_array_equal(out[:,2],points[:,2])
        # Outer terrain boundary is not part of the road/ground seam.
        np.testing.assert_array_equal(out[np.arange(12)*8],points[np.arange(12)*8])

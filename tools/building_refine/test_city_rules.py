import unittest
from types import SimpleNamespace
import numpy as np
from shapely.geometry import Polygon
from city_rules import roof_surface,fit_roofs,regularize_heights
from solid_city import shell,audit_shell


class CityRulesTest(unittest.TestCase):
    def test_stepped_roof_has_no_invented_ramps(self):
        height=np.full((40,40),15.,float);height[:,20:]=19.
        raster=SimpleNamespace(height=height,min_x=0,max_y=20,res=.5)
        mesh,base,report=roof_surface(Polygon([(2,2),(18,2),(18,18),(2,18)]),19,0,raster,0,0,np.zeros(3))
        self.assertIsNotNone(mesh)
        self.assertFalse(any(audit_shell([mesh]).values()))
        p=(mesh.vertices+mesh.translation)[mesh.draw_calls[0][1]].reshape(-1,3,3)
        n=np.cross(p[:,1]-p[:,0],p[:,2]-p[:,0]);n/=np.linalg.norm(n,axis=1)[:,None]
        self.assertFalse(np.any((abs(n[:,2])>.001)&(abs(n[:,2])<.999)))

    def test_plane_fit_removes_isolated_scan_spikes_but_keeps_slope(self):
        y,x=np.indices((20,20));xy=np.c_[x.ravel(),y.ravel()].astype(float)
        expected=15+xy[:,0]*.25
        noisy=expected.copy();noisy[::17]+=7
        fitted,report=regularize_heights(xy,noisy,np.ones(len(xy),bool),1.)
        self.assertLess(np.median(abs(fitted-expected)),.05)
        self.assertLess(fitted.max(),21)
        self.assertGreater(np.ptp(fitted),4)
        self.assertGreater(report['planes'],0)

    def test_pitched_roof_is_closed_and_not_flattened(self):
        yy,xx=np.indices((30,30));height=15+3*(1-abs(xx-15)/15)
        raster=SimpleNamespace(height=height,min_x=0,max_y=15,res=.5)
        poly=Polygon([(1,1),(12,1),(12,5),(7,5),(7,12),(1,12)])
        mesh,base,report=roof_surface(poly,18,0,raster,0,0,np.zeros(3))
        self.assertIsNotNone(mesh)
        self.assertFalse(any(audit_shell([mesh]).values()))
        self.assertGreater(report['height_range'][1]-report['height_range'][0],1)
        own= shell(poly,0,18,0)
        fitted,roofs,reports=fit_roofs(own,raster,0)
        self.assertTrue(roofs)
        self.assertFalse(any(audit_shell(fitted).values()))
        self.assertAlmostEqual(max(m.vertices[:,2].max()+m.translation[2] for m in own),18)

    def test_missing_roof_evidence_keeps_existing_shape(self):
        raster=SimpleNamespace(height=np.full((30,30),-np.inf),min_x=0,max_y=15,res=.5)
        mesh,base,report=roof_surface(Polygon([(1,1),(12,1),(12,12),(1,12)]),18,0,raster,0,0,np.zeros(3))
        self.assertIsNone(mesh)
        self.assertEqual(base,18)
        self.assertEqual(report['status'],'insufficient_roof_capture')

    def test_unsupported_roof_plane_is_not_accepted_as_flat_roof(self):
        # A tiny patch has samples, but insufficient area for a supported plane.
        raster=SimpleNamespace(height=np.full((30,30),18.),min_x=0,max_y=15,res=.5)
        mesh,base,report=roof_surface(Polygon([(1,1),(3,1),(3,3),(1,3)]),18,0,raster,0,0,np.zeros(3))
        self.assertIsNone(mesh)
        self.assertEqual(report['status'],'insufficient_plane_support')
        self.assertEqual(base,18)

    def test_missing_deck_uses_captured_height_and_preserves_courtyard(self):
        from city_rules import recover_roof_deck
        from envelope_repair import footprint
        triangles=[]
        for x in range(10):
            for y in range(10):
                if 3<=x<7 and 3<=y<7:continue
                q=np.array([[x,y,10],[x+1,y,10],[x+1,y+1,10],[x,y+1,10]])
                triangles.extend(q[[0,1,2,0,2,3]].reshape(-1,3,3))
        capture=np.array(triangles,float)
        deck,report=recover_roof_deck([],capture,13,0,0)
        self.assertEqual(report['status'],'captured_deck')
        self.assertAlmostEqual(report['height'],10)
        self.assertFalse(footprint(deck).contains(Polygon([(4,4),(6,4),(6,6),(4,6)])))
        self.assertFalse(any(audit_shell(deck).values()))
        again,result=recover_roof_deck(deck,capture,13,0,0)
        self.assertEqual(again,[])
        self.assertEqual(result['status'],'existing_deck_present')


if __name__=='__main__' :unittest.main()

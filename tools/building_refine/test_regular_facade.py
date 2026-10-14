"""Architectural layout must survive scan fragmentation without editing source."""
import unittest
import numpy as np
import facade
import mtscene
import regular_facade
from test_pcg import fingerprint,mesh_from_model,model


class RegularFacadeTest(unittest.TestCase):
    def test_closed_outline_joins_corners_and_rejects_concave_shape(self):
        from shapely.geometry import box
        def fixture(outline):
            triangles=[];planes=[]
            for a,b in zip(outline,outline[1:]+outline[:1]):
                a=np.array(a,float);b=np.array(b,float)
                delta=b-a;n=np.array([delta[1],-delta[0],0.]);n/=np.linalg.norm(n)
                planes.append(dict(n=n,region=box(0,0,np.linalg.norm(delta),30)))
                for z in range(0,30,3):
                    aa=[*a,z];bb=[*b,z];cc=[*b,z+3];dd=[*a,z+3]
                    triangles.extend([[aa,bb,cc],[aa,cc,dd]])
            return planes,np.array(triangles,float)
        planes,points=fixture([(0,0),(20,0),(20,20),(0,20)])
        shell=regular_facade.close_supported_envelope(planes,points,0)
        self.assertIsNotNone(shell);self.assertEqual(len(shell),4)
        from collections import Counter
        endpoints=[]
        for p in shell:
            left,_,right,_=p['region'].bounds
            endpoints.extend([tuple(np.round(p['offset']*p['n']+x*p['u'],6)) for x in (left,right)])
        self.assertEqual(set(Counter(endpoints).values()),{2})
        planes,points=fixture([(0,0),(20,0),(20,6),(6,6),(6,20),(0,20)])
        self.assertIsNone(regular_facade.close_supported_envelope(planes,points,0))

    def test_competing_planes_do_not_duplicate_windows_and_setbacks_survive(self):
        from shapely.geometry import box
        def plane(y, region, support):
            return dict(n=np.array([0.,1.,0.]),u=np.array([-1.,0.,0.]),
                        v=np.array([0.,0.,1.]),offset=y,region=region,raw_area=support)
        main=plane(0,box(0,0,10,20),200)
        duplicate=plane(1,box(2,0,8,20),120)
        setback=plane(-6,box(2,0,8,20),100)
        wing=plane(1,box(12,0,16,20),80)
        result,removed=regular_facade.resolve_overlaps([duplicate,wing,setback,main],.8)
        self.assertAlmostEqual(removed,120.)
        self.assertEqual(len(result),3)
        self.assertEqual({p['offset'] for p in result},{0,-6,1})
        self.assertAlmostEqual(sum(p['region'].area for p in result),400.)

    def test_fitting_sill_height_preserves_three_floor_band_cadence(self):
        source=mesh_from_model(model(),0)
        pattern={'floor':{'spacing':3.,'anchor_z':0.},'group':{'floors':3,'anchor_z':0.}}
        meshes,_,_=facade.build([source],0,0,parameters={'sill_fraction':.1},detected_pattern=pattern)
        panes=np.concatenate([m.vertices+m.translation for m in meshes if m.material==1]).reshape(-1,6,3)
        bottoms=panes[:,:,2].min(1)
        # Level 0 is a storefront with its own low sill; the fitted sill and
        # the three-floor band are checked on the upper rows (level 3 is a band).
        for level in range(1,4):
            values=bottoms[(bottoms>=level*3)&(bottoms<(level+1)*3)]-level*3
            self.assertGreater(len(values),0)
            self.assertAlmostEqual(float(np.median(values)),.615 if level%3==0 else .365,places=5)

    def test_noisy_coplanar_pieces_become_one_plane_with_regular_windows(self):
        # Independent little scan tiles, with small depth and normal errors.
        triangles=[]
        for x in range(10):
            for z in range(12):
                depth=.08*np.sin(x+z)
                a=np.array([x,depth,z]);b=np.array([x+1,depth+.02,z])
                c=np.array([x+1,depth-.01,z+1]);d=np.array([x,depth+.01,z+1])
                triangles.extend([a,b,c,a,c,d])
        vertices=np.array(triangles,np.float32)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,np.arange(len(vertices),dtype=np.uint32))])
        before=fingerprint(source)
        proxy,planes,stats=regular_facade.rebuild([source])
        self.assertEqual(len(planes),1)
        self.assertEqual(stats['replaced_capture_triangles'],240)
        generated,_,result=facade.build([source],0,0,parameters={'regular_facades':True})
        self.assertGreater(result['windows'],12)
        self.assertEqual(result['patches'],1)
        glass=np.concatenate([m.vertices+m.translation for m in generated if m.material==1])
        # Window panes share one recovered plane; only the entrance door leaf
        # sits 26 cm deeper than the 14 cm pane recess.
        y=glass[:,1];levels,counts=np.unique(np.round(y,2),return_counts=True)
        panes=y[np.abs(y-levels[counts.argmax()])<.05]
        self.assertGreater(len(panes),.8*len(y))
        self.assertLess(np.ptp(panes),.02)
        self.assertLess(np.ptp(y),.27)
        self.assertEqual(before,fingerprint(source))

    def test_large_opening_and_roof_remain(self):
        source=mesh_from_model(model(),0)
        # The full model's horizontal roof must remain a captured surface.
        proxy,planes,_=regular_facade.rebuild([source])
        roofs=[m for m in proxy if getattr(m,'pcg_keep',False)]
        self.assertTrue(roofs)
        for mesh in roofs:
            for _,idx in mesh.draw_calls:
                p=mesh.vertices[idx.reshape(-1,3)]
                self.assertTrue(np.allclose(p[:,:,2],12))
        def wall(x0,x1):
            return [[x0,0,0],[x1,0,0],[x1,0,12],[x0,0,0],[x1,0,12],[x0,0,12]]
        vertices=np.array(wall(0,5)+wall(8,13),np.float32)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,np.arange(12,dtype=np.uint32))])
        generated,_,_=facade.build([source],0,0,parameters={'regular_facades':True})
        for m in generated:
            p=m.vertices+m.translation
            self.assertFalse(((p[:,0]>5.01)&(p[:,0]<7.99)).any())


if __name__=='__main__':unittest.main()

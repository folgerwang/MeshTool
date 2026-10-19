import unittest
import numpy as np
from facade_rectify import regularize, rectify


def opening(x,y,jitter=0,kind='window'):
    return dict(bounds=[x,y+jitter,x+1.2,y+2+jitter],pixel_rect=[0,0,1,1],kind=kind,shape='rectangular')


class FacadeRectifyTest(unittest.TestCase):
    def test_flush_frame_stays_on_wall_and_glass_is_shallow(self):
        from building_rules import opening_template
        from window_detail import geometry
        parts=opening_template(1.2,2.,'window',surface_mode='flush')
        self.assertTrue(np.all(parts['frame'][:,2]==0))
        np.testing.assert_allclose(parts['glazing'][:,2],-.012)
        bars=geometry(1.2,2.,'rectangular',[dict(axis='x',position=.5,width_m=.05)],.012)
        self.assertLess(bars[:,2].max(),0)
        self.assertGreater(bars[:,2].min(),-.012)

    def test_window_has_frame_glass_and_closed_recessed_backing(self):
        from building_rules import opening_template, material_for_part
        from solid_city import audit_shell
        import mtscene
        parts=opening_template(1.2,2.,'window')
        self.assertTrue(len(parts['frame']))
        self.assertLess(parts['interior'][:,2].max(),parts['glazing'][:,2].min())
        points=parts['interior']
        mesh=mtscene.Mesh(0,0,np.zeros(3),points,np.zeros((len(points),2)),None,[(4,np.arange(len(points),dtype=np.uint32))])
        self.assertFalse(any(audit_shell([mesh]).values()))
        self.assertEqual(material_for_part('interior'),mtscene.MAT_INTERIOR)
        self.assertEqual(material_for_part('glazing'),mtscene.MAT_GLASS)
        self.assertNotIn('interior',opening_template(1.,2.,'door_candidate'))

    def test_rows_straighten_without_losing_staggered_pattern(self):
        items=[opening(1+i*2,1,delta) for i,delta in enumerate([-.08,0,.08])]
        items += [opening(2+i*2,4,delta) for i,delta in enumerate([.08,0,-.08])]
        corrected=regularize(items)
        self.assertEqual(len(corrected),6)
        for a,b in zip(items,corrected):self.assertEqual(a['bounds'][::2],b['bounds'][::2])
        self.assertEqual([i['bounds'][1] for i in corrected],[1,1,1,4,4,4])

    def test_doors_and_large_outliers_are_not_moved(self):
        items=[opening(i*2,1,j) for i,j in enumerate([-.08,0,.08,.3])]+[opening(0,0,0,'door_candidate')]
        out=regularize(items)
        self.assertEqual(out[-1],items[-1]);self.assertEqual(out[3],items[3])

    def test_warp_preserves_border_and_repeated_iteration_converges(self):
        yy,xx=np.indices((100,100));image=np.stack([xx,yy,xx],axis=-1).astype(np.uint8)
        items=[opening(1+i*2,2,j) for i,j in enumerate([-.08,0,.08])]
        warped,updated,report=rectify(image,items,.1)
        self.assertEqual(report['status'],'rectified')
        np.testing.assert_array_equal(warped[0],image[0]);np.testing.assert_array_equal(warped[:,-1],image[:,-1])
        _,again,second=rectify(warped,updated,.1)
        self.assertEqual(second['changed_windows'],0)
        self.assertEqual(updated,again)


if __name__=='__main__':unittest.main()

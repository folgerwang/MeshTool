import unittest
import numpy as np
from regular_window_layout import fit_floors,fit_columns,openings,profile_openings


class RegularWindowLayoutTest(unittest.TestCase):
    def test_floor_fit_ignores_ground_outliers_and_balances_facades(self):
        instances=[]
        for fid,offset in [(0,.1),(1,-.1)]:
            for row in range(4):
                for column in range(20 if fid==0 else 2):
                    y=7+row*3+offset
                    instances.append(dict(kind='window',face_id=fid,bounds=[column,y-.8,column+1,y+.8]))
        instances.append(dict(kind='window',face_id=0,bounds=[0,1,1,2]))
        fit=fit_floors(dict(instances=instances),{0,1},[7,3])
        self.assertAlmostEqual(fit['first_center_m'],7)
        self.assertAlmostEqual(fit['spacing_m'],3)
        self.assertEqual(fit['face_row_observations'],8)

    def test_paired_motif_recovers_missing_member_without_uniform_single_grid(self):
        fit=fit_columns([[0,0,2],[0,1,3.5],[1,0,7],[2,0,12],[2,1,13.5]],3)
        np.testing.assert_allclose(fit['centers_m'],[2,3.5,7,8.5,12,13.5])
        slots,skipped=openings(dict(first_center_m=3,spacing_m=3,count=4),fit,1,1.8,[16,15],[188,200,3],.08)
        self.assertEqual(len(slots),24);self.assertEqual(skipped,[])
        for column in range(6):
            centers=[(o['bounds'][0]+o['bounds'][2])/2 for o in slots if o['layout_column']==column]
            np.testing.assert_allclose(centers,np.full(4,fit['centers_m'][column]))

    def test_corner_slots_are_omitted_instead_of_shrunk(self):
        slots,skipped=openings(dict(first_center_m=3,spacing_m=3,count=2),dict(centers_m=[1,4.9]),1,1.5,[5,8],[100,63,3],.08)
        self.assertEqual(len(slots),2);self.assertEqual(len(skipped),2)
        self.assertTrue(all(o['bounds'][2]-o['bounds'][0]==1 for o in slots))

    def test_overlapping_motif_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'overlap'):
            openings(dict(first_center_m=3,spacing_m=3,count=2),dict(centers_m=[1,1.5]),1,1.5,[5,8],[100,63,3],.08)

    def test_unsupported_floor_pattern_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'Insufficient'):
            fit_floors(dict(instances=[]),{0},[7,3])

    def test_mixed_glazing_and_service_windows_share_floor_grid(self):
        groups=[dict(bay_count=1,column_observations=[[0,0,2]],window_width_m=2.4),
                dict(bay_count=1,column_observations=[[0,0,5]],window_width_m=.7,
                     window_height_m=1.,floor_offset_m=.4)]
        slots,report=profile_openings(dict(first_center_m=3,spacing_m=3,count=2),
            dict(column_groups=groups),2.,[8,10],[125,100,3],.08)
        self.assertEqual(len(slots),4)
        np.testing.assert_allclose([o['bounds'][2]-o['bounds'][0] for o in slots],[2.4,.7,2.4,.7])
        np.testing.assert_allclose([(o['bounds'][1]+o['bounds'][3])/2 for o in slots],[3,3.4,6,6.4])
        self.assertEqual(report['slots'],4)

    def test_independent_column_groups_must_not_overlap(self):
        group=dict(bay_count=1,column_observations=[[0,0,2]],window_width_m=2.)
        with self.assertRaisesRegex(ValueError,'groups overlap'):
            profile_openings(dict(first_center_m=3,spacing_m=3,count=2),
                dict(column_groups=[group,group]),2.,[8,10],[125,100,3],.08)

    def test_single_motif_profile_retains_existing_slots(self):
        floors=dict(first_center_m=3,spacing_m=3,count=2)
        config=dict(bay_count=2,column_observations=[[0,0,2],[1,0,5]],window_width_m=1.)
        expected,_=openings(floors,fit_columns(config['column_observations'],2),1.,2.,[8,10],[125,100,3],.08)
        actual,_=profile_openings(floors,config,2.,[8,10],[125,100,3],.08)
        self.assertEqual(actual,expected)


if __name__=='__main__':unittest.main()

"""Nine-view alignment, absolute acceptance gates, and source preservation."""
import contextlib
import copy
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np

import facade
import mtscene
import multiview
import raster
import refine
from test_pcg import mesh_from_model, model, fingerprint


def source():
    mesh=mesh_from_model(model(),0)
    mesh.tex_index=0
    mesh.uvs=np.zeros((len(mesh.vertices),2),np.float32)
    rgb=np.full((32,32,3),88,np.uint8)
    texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,
                            [(32,32,raster.encode_dxt1(rgb))])
    return [mesh],[texture]


class CameraFitTest(unittest.TestCase):
    def test_structure_metric_penalizes_shifted_window_rows(self):
        meshes,textures=source();cameras=multiview.Cameras(meshes,textures,128)
        for image,_,_ in cameras.reference:
            image[:]=np.where((np.indices(image.shape[:2])[0]//4)%2,180,40)[:,:,None]
        identical=cameras.scores(cameras.reference)
        shifted=[(np.roll(image,4,axis=0),depth,ids) for image,depth,ids in cameras.reference]
        wrong=cameras.scores(shifted)
        self.assertTrue(all(s['structure']<1e-6 for s in identical))
        self.assertTrue(all(s['structure']>.4 for s in wrong))

    def test_fixed_cameras_identity_and_displaced_shape(self):
        meshes,textures=source()
        cameras=multiview.Cameras(meshes,textures,128)
        scores=cameras.scores(cameras.render(meshes,textures))
        self.assertTrue(all(s['loss']==0 and s['silhouette_iou']==1 for s in scores))
        moved=copy.deepcopy(meshes)
        moved[0].translation[0]+=3
        worse=cameras.scores(cameras.render(moved,textures))
        self.assertEqual(len(cameras.bases),9)
        self.assertGreater(multiview.aggregate(worse,multiview.ALL_VIEWS),.01)
        self.assertTrue(all(v.any() for v in cameras.valid))
        np.testing.assert_allclose(cameras.bases[multiview.TOP_VIEW][:,2],[0,0,1])
        np.testing.assert_allclose(cameras.bases[0][:,2],-cameras.bases[1][:,2])
        np.testing.assert_allclose(cameras.bases[2][:,2],-cameras.bases[3][:,2])
        for basis in cameras.bases:
            np.testing.assert_allclose(basis.T @ basis,np.eye(3),atol=1e-7)
        for i in range(4,8):
            self.assertAlmostEqual(cameras.bases[i][2,2],np.sin(np.deg2rad(20)))
        directions=np.array([b[:,2] for b in cameras.bases])
        self.assertEqual(len(np.unique(directions.round(6),axis=0)),9)

    def test_each_view_must_pass_absolute_error_and_shape_limits(self):
        base=[dict(loss=.4,photo=.2,pixels=128,silhouette_iou=.99,depth_fraction=.005) for _ in multiview.ALL_VIEWS]
        better=[dict(loss=.1,photo=.1,pixels=128,silhouette_iou=.99,depth_fraction=.005) for _ in multiview.ALL_VIEWS]
        self.assertTrue(multiview.accepts(base,better))
        for i in multiview.ALL_VIEWS:
            failed=copy.deepcopy(better);failed[i]['loss']=.21
            self.assertFalse(multiview.accepts(base,failed))
            self.assertTrue(any(multiview.VIEW_NAMES[i] in f for f in multiview.failures(failed)))
        for field,value in [('silhouette_iou',.89),('depth_fraction',.03),('pixels',0),('loss',float('nan'))]:
            failed=copy.deepcopy(better);failed[multiview.TOP_VIEW][field]=value
            self.assertFalse(multiview.accepts(base,failed),field)
        self.assertFalse(multiview.accepts(base,better[:4]))
        self.assertFalse(multiview.accepts(base,base)) # Improvement alone cannot accept a poor model.

    def test_identical_model_stops_without_unnecessary_iterations(self):
        meshes,textures=source()
        baseline=(meshes,[],dict(windows=4,floor=3,bay=4))
        with patch('facade.build',side_effect=AssertionError('already acceptable')),contextlib.redirect_stdout(io.StringIO()):
            result=multiview.fit(meshes,0,1,textures,'painted-concrete',baseline,3)
        self.assertTrue(result[2]['multiview']['accepted'])
        self.assertEqual(len(result[2]['multiview']['history']),1)

    def test_top_camera_detects_wrong_roof_height_with_matching_outline(self):
        meshes,textures=source();cameras=multiview.Cameras(meshes,textures,128)
        candidate=copy.deepcopy(meshes)
        candidate[0].vertices[candidate[0].vertices[:,2]>11.9,2]+=2
        scores=cameras.scores(cameras.render(candidate,textures))
        self.assertGreater(scores[multiview.TOP_VIEW]['silhouette_iou'],.99)
        self.assertGreater(scores[multiview.TOP_VIEW]['depth_mae_m'],1.9)
        self.assertTrue(any(f.startswith('top:') for f in multiview.failures(scores)))

    def test_passing_all_views_beats_a_better_average_with_one_bad_side(self):
        good=[dict(loss=.19,photo=.1,pixels=128,silhouette_iou=.99,depth_fraction=.005) for _ in multiview.ALL_VIEWS]
        bad=copy.deepcopy(good)
        for s in bad:s['loss']=.01
        bad[multiview.TOP_VIEW]['loss']=.25
        self.assertLess(multiview.aggregate(bad,multiview.ALL_VIEWS),multiview.aggregate(good,multiview.ALL_VIEWS))
        self.assertLess(multiview.rank(good),multiview.rank(bad))

    def test_bounded_iterations_report_all_nine_views_and_preserve_source(self):
        meshes,textures=source()
        before=fingerprint(meshes[0])
        baseline=facade.build(meshes,0,1,textures)
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()):
            result=multiview.fit(meshes,0,1,textures,'painted-concrete',baseline,2,d,max_error=.000001)
            report=result[2]['multiview']
            self.assertFalse(report['accepted'])
            self.assertEqual({h['iteration'] for h in report['history']},{0,1,2})
            self.assertTrue((Path(d)/'cameras.png').is_file())
            self.assertTrue((Path(d)/'fit.json').is_file())
            self.assertEqual(report['camera_count'],9)
            self.assertEqual([v['name'] for v in report['views']],list(multiview.VIEW_NAMES))
            self.assertEqual(len(report['candidate']),9)
            for name in multiview.VIEW_NAMES:
                self.assertTrue((Path(d)/f'{name}_capture.png').is_file())
                self.assertTrue((Path(d)/f'{name}_fitted_candidate.png').is_file())
            self.assertLessEqual(multiview.rank(report['candidate'],.000001),
                                 multiview.rank(report['baseline'],.000001))
        self.assertEqual(before,fingerprint(meshes[0]))

    def test_rejected_building_is_not_applied_or_given_new_textures(self):
        import test_pcg
        batches=test_pcg.fixture();group=batches[0].groups[0]
        before=[fingerprint(m) for m in group.meshes];textures=list(group.textures)
        def reject(*args,**kwargs):
            new,tex,stats=args[5]
            stats['multiview']=dict(accepted=False,failed_checks=['top: error exceeds limit'])
            return new,tex,stats
        with patch('multiview.fit',side_effect=reject),contextlib.redirect_stdout(io.StringIO()):
            done,total=facade.apply(batches,(0,0,0),fit_iterations=2)
        self.assertEqual((done,total),(0,1))
        self.assertEqual(before,[fingerprint(m) for m in group.meshes])
        self.assertEqual(textures,group.textures)

    def test_untextured_capture_does_not_claim_fit(self):
        meshes,_=source();meshes[0].uvs=None
        baseline=facade.build(meshes,0,0)
        with contextlib.redirect_stdout(io.StringIO()):
            result=multiview.fit(meshes,0,0,[],'painted-concrete',baseline,2)
        self.assertIs(result[0],baseline[0])
        self.assertFalse(result[2]['multiview']['eligible'])
        self.assertFalse(result[2]['multiview']['accepted'])

    def test_refine_rejects_source_overwrite_before_loading(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/'source.mtscene';path.write_bytes(b'preserve me')
            with patch('sys.argv',['refine',str(path),str(path),'--tiled-facade']),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:refine.main()
            self.assertEqual(error.exception.code,2)
            self.assertEqual(path.read_bytes(),b'preserve me')


if __name__=='__main__':unittest.main()

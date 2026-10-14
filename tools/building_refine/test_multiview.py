"""Camera-fit checks: image alignment, held-out rejection, and source safety."""
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
        self.assertGreater(multiview.aggregate(worse,range(8)),.1)
        self.assertTrue(all(v.any() for v in cameras.valid))

    def test_validation_regression_rejects_training_gain(self):
        base=[dict(loss=.2,silhouette_iou=.99,depth_mae_m=.1) for _ in range(8)]
        better=[dict(loss=.1,silhouette_iou=.99,depth_mae_m=.1) for _ in range(8)]
        self.assertTrue(multiview.accepts(base,better))
        for i in range(1,8,2):better[i]['loss']=.21
        self.assertFalse(multiview.accepts(base,better))
        better=copy.deepcopy(base)
        for b in better:b['loss']=.1
        better[0]['silhouette_iou']=.98
        self.assertFalse(multiview.accepts(base,better))

    def test_two_iterations_improve_real_geometry_and_preserve_source(self):
        meshes,textures=source()
        before=fingerprint(meshes[0])
        baseline=facade.build(meshes,0,1,textures)
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stdout(io.StringIO()):
            result=multiview.fit(meshes,0,1,textures,'painted-concrete',baseline,2,d)
            report=result[2]['multiview']
            self.assertTrue(report['accepted'])
            self.assertEqual({h['iteration'] for h in report['history']},{0,1,2})
            self.assertTrue((Path(d)/'cameras.png').is_file())
            self.assertTrue((Path(d)/'fit.json').is_file())
            self.assertLess(multiview.aggregate(report['candidate'],range(1,8,2)),
                            multiview.aggregate(report['baseline'],range(1,8,2)))
        self.assertEqual(before,fingerprint(meshes[0]))

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

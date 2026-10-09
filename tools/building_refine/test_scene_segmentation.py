"""Native scene SaveScene/LoadScene regression: embedded segmentation is lossless."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import numpy as np
import building
import mtscene
import pcg
import raster
import refine
import test_pcg

EXE=Path(os.environ.get('MESHTOOL_TEST_EXE',str(Path(__file__).resolve().parents[2]/'build/Release/MeshTool.exe')))

class SceneSegmentationTest(unittest.TestCase):
    def roundtrip(self,batches):
        with tempfile.TemporaryDirectory() as d:
            source,target=Path(d)/'source.mtscene',Path(d)/'restored.mtscene'
            mtscene.save(source,batches)
            run=subprocess.run([str(EXE),'--objects',str(source),str(target)],timeout=30,stdout=subprocess.DEVNULL)
            self.assertEqual(run.returncode,0)
            self.assertTrue(target.exists())
            # This calls the real native loader and writer. Equality covers
            # class/name tables, object assignments, geometry, UVs, textures,
            # GPS and refinement material tags, not just a Python serializer.
            self.assertEqual(source.read_bytes(),target.read_bytes())
            return mtscene.load(target)

    def test_all_classes_duplicate_names_and_unassigned_meshes(self):
        b=test_pcg.fixture(); g=b[0].groups[0]
        for cls in (0,2,4,5,6,7): g.objects.append(mtscene.SceneObject('class_'+str(cls),cls))
        g.objects[0].name='building_建筑'
        b[0].groups[1].objects[0].name=g.objects[0].name
        g.meshes.append(test_pcg.mesh_from_model(test_pcg.model(75),-1))
        out=self.roundtrip(b)
        self.assertEqual(g.objects[0].name,out[0].groups[0].objects[0].name)
        self.assertEqual([m.object_id for m in g.meshes],[m.object_id for m in out[0].groups[0].meshes])

    def test_refined_building_labels_textures_and_pbr_survive_native_save(self):
        b=test_pcg.fixture(); g=b[0].groups[0]; model=test_pcg.model()
        class Glass:
            def p_glass(self,crops): return np.ones(len(crops))
        meshes,textures,*_=refine.model_meshes(model,building.model_faces(model),raster.collect(g),
                                              raster.decode_textures(g),.5,0,0,Glass())
        details,_=pcg.detail_meshes(building.model_faces(model),0,model)
        g.meshes=[m for m in g.meshes if m.object_id!=0]+meshes+details
        g.textures=textures
        out=self.roundtrip(b)
        mats={m.material for m in out[0].groups[0].meshes if m.object_id==0}
        self.assertTrue({1,2,4,5,6}.issubset(mats))
        self.assertGreater(len(out[0].groups[0].textures),0)

if __name__=='__main__': unittest.main()

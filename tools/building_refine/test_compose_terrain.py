import copy
import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
import mtscene
from compose_terrain import compose,digest
from test_pcg import fixture


class CompositionTest(unittest.TestCase):
    def setup_files(self,root):
        source=mtscene.Scene(fixture());source[0].groups=source[0].groups[:1]
        mtscene.save(root/'source.mtscene',source)
        library=copy.deepcopy(source);g=library[0].groups[0]
        g.meshes[0].model_variant=1
        pcg=copy.deepcopy(g.meshes[0]);pcg.model_variant=2
        pcg.translation=np.array([0.,0.,.5]);g.meshes.append(pcg)
        mtscene.save(root/'library.mtscene',library)
        terrain=copy.deepcopy(source);tg=terrain[0].groups[0]
        tg.meshes=tg.meshes[2:];tg.meshes[0].object_id=0;tg.objects=tg.objects[2:]
        mtscene.save(root/'terrain.mtscene',terrain)
        manifest=dict(source=str(root/'source.mtscene'),source_sha256=digest(root/'source.mtscene'),scene='terrain.mtscene',
            groups=[dict(source_batch=0,source_group=0,output_batch=0,output_group=0,placements=[
                dict(source_object=i,name=o.name,kind='building') for i,o in enumerate(g.objects[:2])])])
        (root/'manifest.json').write_text(json.dumps(manifest))
        return library

    def test_pcg_only_preserves_world_coordinates_and_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);library=self.setup_files(root)
            before={p:digest(p) for p in root.iterdir()}
            report=compose(root/'manifest.json',root/'library.mtscene',root/'out')
            result=mtscene.load(root/'out/terrain_pcg.mtscene');g=result[0].groups[0]
            self.assertEqual([o.cls for o in g.objects],[1,3])
            self.assertEqual(len(g.meshes),2)
            np.testing.assert_array_equal(g.meshes[1].translation,library[0].groups[0].meshes[-1].translation)
            self.assertEqual(len(report['pending_pcg']),1)
            self.assertEqual(before,{p:digest(p) for p in before})
            with self.assertRaises(ValueError):compose(root/'manifest.json',root/'library.mtscene',root/'out')

    def test_reject_unrelated_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);library=self.setup_files(root)
            library[0].groups[0].meshes[0].vertices[0,0]+=1
            mtscene.save(root/'library.mtscene',library)
            with self.assertRaisesRegex(ValueError,'originals do not match'):
                compose(root/'manifest.json',root/'library.mtscene',root/'out')
            self.assertFalse((root/'out').exists())

"""Original geometry must survive refinement, retries and scene round trips."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
import os
import struct
import subprocess
from unittest.mock import patch

import mtscene
import test_pcg
import refine


def run(batches):
    with tempfile.TemporaryDirectory() as d:
        source,target=Path(d)/'input.mtscene',Path(d)/'output.mtscene'
        mtscene.save(source,batches)
        with patch('sys.argv',['refine',str(source),str(target),'--pcg','--target','0:0:0','--no-sam','--no-glass']),contextlib.redirect_stdout(io.StringIO()):
            assert refine.main()==0
        return mtscene.load(target)


class ModelComparisonTest(unittest.TestCase):
    def test_legacy_v4_still_loads_and_native_save_preserves_comparison_pair(self):
        exe=Path(os.environ.get('MESHTOOL_TEST_EXE',str(Path(__file__).resolve().parents[2]/'build/Release/MeshTool.exe')))
        name=b'legacy_building'
        data=mtscene.MAGIC+struct.pack('<IIBBBddII',4,1,0,1,1,-122.,47.,1,0)
        data+=struct.pack('<IIIiBdddBB',1,3,0xffffffff,0,0,0.,0.,0.,0,0)
        data+=struct.pack('<9f',0,0,0,1,0,0,0,1,0)
        data+=struct.pack('<6I',1,4,3,0,1,2)
        data+=struct.pack('<II',1,len(name))+name+bytes([3])
        with tempfile.TemporaryDirectory() as d:
            old,new=Path(d)/'legacy.mtscene',Path(d)/'new.mtscene'
            old.write_bytes(data)
            self.assertEqual(mtscene.load(old)[0].groups[0].meshes[0].model_variant,0)
            self.assertEqual(subprocess.run([str(exe),'--objects',str(old),str(new)],stdout=subprocess.DEVNULL,timeout=30).returncode,0)
            self.assertEqual(mtscene.load(new)[0].groups[0].objects[0].name,'legacy_building')
            pair=run(test_pcg.fixture())
            mtscene.save(old,pair)
            self.assertEqual(subprocess.run([str(exe),'--objects',str(old),str(new)],stdout=subprocess.DEVNULL,timeout=30).returncode,0)
            self.assertEqual(old.read_bytes(),new.read_bytes())

    def test_pair_retains_exact_original_and_separate_refined_geometry(self):
        source=test_pcg.fixture()
        before=source[0].groups[0].meshes[0]
        result=run(source)
        group=result[0].groups[0]
        original=[m for m in group.meshes if m.object_id==0 and m.model_variant==1]
        refined=[m for m in group.meshes if m.object_id==0 and m.model_variant==2]
        self.assertEqual(len(original),1)
        before.model_variant=1
        self.assertEqual(test_pcg.fingerprint(original[0]),test_pcg.fingerprint(before))
        self.assertTrue(refined)
        self.assertTrue(all(m.model_variant!=0 for m in group.meshes if m.object_id==0))
        self.assertTrue(all(m.model_variant==0 for m in group.meshes if m.object_id!=0))

    def test_repeat_refinement_keeps_first_original_without_stacking(self):
        first=run(test_pcg.fixture()); second=run(first)
        def originals(b): return [test_pcg.fingerprint(m) for m in b[0].groups[0].meshes if m.model_variant==1]
        self.assertEqual(originals(first),originals(second))
        self.assertEqual(len(originals(second)),1)

    def test_rejected_fit_keeps_existing_pair(self):
        first=run(test_pcg.fixture())
        with patch('building.build_model',return_value=None): second=run(first)
        def meshes(b): return sorted(test_pcg.fingerprint(m) for m in b[0].groups[0].meshes)
        self.assertEqual(meshes(first),meshes(second))

    def test_initial_rejection_preserves_shell_with_pbr_fallback(self):
        with patch('building.build_model',return_value=None): result=run(test_pcg.fixture())
        group=result[0].groups[0]
        original=[m for m in group.meshes if m.object_id==0 and m.model_variant==1]
        shell=[m for m in group.meshes if m.object_id==0 and m.model_variant==2 and m.material==3]
        self.assertEqual(len(original),1)
        self.assertEqual(len(shell),1)
        import numpy as np
        np.testing.assert_array_equal(original[0].vertices,shell[0].vertices)
        np.testing.assert_array_equal(original[0].draw_calls[0][1],shell[0].draw_calls[0][1])
        self.assertTrue(all(m.model_variant==0 for m in group.meshes if m.object_id!=0))

if __name__=='__main__': unittest.main()

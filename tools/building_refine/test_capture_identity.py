import copy
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import mtscene
from capture_identity import ensure_capture
from remove_trees import remove, retained_state
from test_pcg import fixture


class CaptureIdentityTest(unittest.TestCase):
    def test_new_captures_get_unique_names_and_stable_tree_ownership(self):
        scene = mtscene.Scene(fixture()); other = mtscene.Scene(fixture())
        identity = copy.deepcopy(ensure_capture(scene))
        self.assertRegex(identity['name'], r'^capture_\d{8}T\d{6}Z_[0-9a-f]{8}$')
        self.assertNotEqual(identity['id'], ensure_capture(other)['id'])
        scene[0].groups[0].objects[0].cls = other[0].groups[0].objects[0].cls = 5
        a, b = remove(scene)[0], remove(other)[0]
        self.assertEqual(a['capture_id'], identity['id'])
        self.assertNotEqual(a['instance_id'], b['instance_id'])
        self.assertEqual(ensure_capture(scene), identity)

    def test_legacy_copies_get_same_identity_and_foreign_records_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root/'old.mtscene'; copy_path = root/'renamed.mtscene'
            mtscene.save(source, fixture()); shutil.copyfile(source, copy_path)
            a, b = mtscene.load(source), mtscene.load(copy_path)
            self.assertEqual(ensure_capture(a, source)['id'], ensure_capture(b, copy_path)['id'])
            a[0].groups[0].tree_instances = [{'capture_id': 'different-capture'}]
            before = copy.deepcopy(a[0].groups[0].tree_instances)
            with self.assertRaisesRegex(ValueError, 'different capture'): ensure_capture(a)
            self.assertEqual(a[0].groups[0].tree_instances, before)

    def test_identity_and_route_survive_renaming_and_truncation_is_rejected(self):
        scene = mtscene.Scene(fixture()); ensure_capture(scene)
        with tempfile.TemporaryDirectory() as temp:
            source, renamed = Path(temp)/'a.mtscene', Path(temp)/'renamed.mtscene'
            mtscene.save(source, scene, dict(polyline=[(-122.,47.),(-122.1,47.1)]))
            loaded = mtscene.load(source); mtscene.save(renamed, loaded)
            self.assertEqual(source.read_bytes(), renamed.read_bytes())
            self.assertEqual(mtscene.load(renamed).capture, scene.capture)
            renamed.write_bytes(source.read_bytes()[:-1])
            with self.assertRaises(ValueError): mtscene.load(renamed)

    @unittest.skipUnless(os.environ.get('MESHTOOL_TEST_EXE'), 'Set MESHTOOL_TEST_EXE for native integration')
    def test_native_bundle_can_move_and_preserves_all_scene_data(self):
        exe = str(Path(os.environ['MESHTOOL_TEST_EXE']).resolve())
        scene = mtscene.Scene(fixture()); ensure_capture(scene)
        scene[0].groups[0].objects[0].cls = 5
        remove(scene)
        # Export is not a geometry cleanup: coincident meshes must survive too.
        scene[0].groups[0].meshes.append(copy.deepcopy(scene[0].groups[0].meshes[0]))
        with tempfile.TemporaryDirectory(prefix='capture folder ') as temp:
            root = Path(temp); source = root/'input.mtscene'; parent = root/'captures'
            mtscene.save(source, scene, dict(polyline=[(-122.,47.),(-122.1,47.1)]))
            original = mtscene.load(source)
            result = subprocess.run([exe, '--save-capture', str(source), str(parent)], timeout=60)
            self.assertEqual(result.returncode, 0)
            folder = parent/scene.capture['name']
            manifest = json.loads((folder/'capture.json').read_text())
            self.assertEqual(manifest['capture'], scene.capture)
            saved = mtscene.load(folder/manifest['scene_file'])
            self.assertEqual(saved.capture, original.capture)
            self.assertEqual(saved.route_data, original.route_data)
            self.assertEqual(retained_state(saved), retained_state(original))
            catalog = json.loads((folder/manifest['trees_file']).read_text())
            self.assertEqual(catalog['capture'], scene.capture)
            self.assertEqual(catalog['instances'], scene[0].groups[0].tree_instances)
            before = (folder/manifest['scene_file']).read_bytes()
            repeat = subprocess.run([exe, '--save-capture', str(source), str(parent)], timeout=60)
            self.assertNotEqual(repeat.returncode, 0)
            self.assertEqual((folder/manifest['scene_file']).read_bytes(), before)
            moved = root/'moved'; shutil.move(str(folder), moved)
            self.assertEqual(mtscene.load(moved/manifest['scene_file']).capture, original.capture)
            # The scene remains complete even without its JSON sidecars.
            standalone = root/'standalone.mtscene'
            shutil.copyfile(moved/manifest['scene_file'], standalone)
            self.assertEqual(retained_state(mtscene.load(standalone)), retained_state(original))
            bad = mtscene.load(source); bad.capture['name'] = '../escape'
            mtscene.save(root/'bad.mtscene', bad)
            result = subprocess.run([exe, '--save-capture', str(root/'bad.mtscene'), str(parent)], timeout=60)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__': unittest.main()

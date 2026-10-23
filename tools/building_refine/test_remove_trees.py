import copy
import unittest
import tempfile
import struct
import json
import os
import subprocess
from pathlib import Path
import numpy as np

import mtscene
from compose_iteration import fingerprint
from remove_trees import remove, retained_state, run
from capture_identity import ensure_capture
from test_pcg import fixture


class RemoveTreesTest(unittest.TestCase):
    def test_bounds_use_referenced_vertices_translation_and_original_variant(self):
        scene = mtscene.Scene(fixture()); g = scene[0].groups[0]
        g.objects[0].cls = 5
        original = g.meshes[0]; original.model_variant = 1
        original.translation = np.array([100., 200., 30.])
        original.vertices = np.vstack([original.vertices, [[9999, 9999, 9999]]]).astype(np.float32)
        refined = copy.deepcopy(original); refined.model_variant = 2; refined.translation += 50
        g.meshes.append(refined)
        records = remove(scene); tree = records[0]
        self.assertEqual(tree['bbox'], dict(min=[100., 200., 30.], max=[112., 210., 42.]))
        self.assertEqual(tree['position'], [106., 205., 30.])
        self.assertEqual(tree['size_m'], [12., 10., 12.])
        self.assertEqual(tree['reference_lon_lat'], [-122.19, 47.61])
        self.assertFalse(tree['ground_verified'])
        self.assertEqual(tree['variant_bounds']['2']['min'], [150., 250., 80.])
        self.assertEqual(g.tree_instances, records)

    def test_invalid_tree_does_not_remove_any_geometry_or_add_partial_records(self):
        scene = mtscene.Scene(fixture()); g = scene[0].groups[0]
        g.objects[0].cls = g.objects[1].cls = 5
        g.meshes[1].vertices[0,0] = np.nan
        before = fingerprint(g.meshes)
        with self.assertRaisesRegex(ValueError, 'Non-finite'): remove(scene)
        self.assertEqual(before, fingerprint(g.meshes))
        self.assertEqual(g.tree_instances, [])

    def test_unassigned_mesh_is_not_mistaken_for_last_tree_object(self):
        scene = mtscene.Scene(fixture()); g = scene[0].groups[0]
        g.objects[-1].cls = 5; g.meshes[0].object_id = -1
        original = fingerprint([g.meshes[0]])
        remove(scene)
        self.assertEqual(fingerprint([g.meshes[0]]), original)

    def test_saved_catalog_roundtrip_and_repeat_removal_keep_planting_data(self):
        scene = mtscene.Scene(fixture()); scene[0].groups[0].objects[0].cls = 5
        route = dict(polyline=[(-122.,47.),(-122.1,47.1)], from_name='start',to_name='end',length_m=100.)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root/'source.mtscene'
            mtscene.save(source, scene, route)
            self.assertEqual(struct.unpack_from('<I',source.read_bytes(),8)[0],7)
            original = mtscene.load(source)
            run(source, root/'first')
            loaded = mtscene.load(root/'first/city.mtscene')
            self.assertEqual(loaded.route_data, original.route_data)
            self.assertEqual(struct.unpack_from('<I',(root/'first/city.mtscene').read_bytes(),8)[0],9)
            catalog = json.loads((root/'first/tree_instances.json').read_text())
            self.assertEqual(catalog['instances'],loaded[0].groups[0].tree_instances)
            run(root/'first/city.mtscene',root/'second')
            again = mtscene.load(root/'second/city.mtscene')
            self.assertEqual(retained_state(again),retained_state(loaded))
            self.assertEqual(json.loads((root/'second/tree_instances.json').read_text())['instances'], catalog['instances'])

    def test_all_tree_variants_removed_and_other_classes_retained(self):
        scene = mtscene.Scene(fixture(), route_data=b'route payload preserved')
        group = scene[0].groups[0]
        template = group.meshes[0]
        group.objects = [mtscene.SceneObject(str(c), c) for c in (1, 2, 3, 4, 5, 6, 7)]
        group.meshes = []
        for oid in range(len(group.objects)):
            for variant in (0, 1, 2):
                mesh = copy.deepcopy(template)
                mesh.object_id, mesh.model_variant = oid, variant
                group.meshes.append(mesh)
        textures = copy.deepcopy(group.textures)
        objects = copy.deepcopy(group.objects)
        expected = fingerprint([m for m in group.meshes if m.object_id != 4])
        records = remove(scene)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]['variants'], [0, 1, 2])
        self.assertEqual(records[0]['meshes'], 3)
        self.assertEqual(fingerprint(group.meshes), expected)
        self.assertEqual(group.textures, textures)
        self.assertEqual(group.objects, objects)
        self.assertEqual(scene.route_data, b'route payload preserved')
        before = retained_state(scene)
        self.assertEqual(remove(scene), [])
        self.assertEqual(retained_state(scene), before)

    def test_every_batch_and_group_is_processed(self):
        scene = mtscene.Scene(fixture())
        scene[0].groups[0].objects[0].cls = 5
        scene[0].groups.append(copy.deepcopy(scene[0].groups[0]))
        scene.append(copy.deepcopy(scene[0]))
        remove(scene)
        self.assertFalse(any(g.objects[m.object_id].cls == 5
                             for b in scene for g in b.groups for m in g.meshes))


@unittest.skipUnless(os.environ.get('MESHTOOL_TEST_EXE'), 'Set MESHTOOL_TEST_EXE for native integration')
class NativeRemoveTreesTest(unittest.TestCase):
    def test_worker_and_native_save_keep_planting_records_and_route(self):
        exe = str(Path(os.environ['MESHTOOL_TEST_EXE']).resolve())
        scene = mtscene.Scene(fixture())
        scene[0].groups[0].objects[0].cls = 5
        route = dict(polyline=[(-122.,47.),(-122.1,47.1)], from_name='start', to_name='end', length_m=100.)
        with tempfile.TemporaryDirectory(prefix='tree planting ') as temp:
            root = Path(temp); source = root/'input.mtscene'; output = root/'output.mtscene'
            mtscene.save(source, scene, route)
            source_bytes = source.read_bytes()
            expected = mtscene.load(source); ensure_capture(expected, source); remove(expected)
            def native(*args):
                result = subprocess.run([exe, *map(str,args)], capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            native('--remove-trees', source, output)
            loaded = mtscene.load(output)
            self.assertEqual(retained_state(loaded), retained_state(expected))
            self.assertEqual(loaded.route_data, expected.route_data)
            self.assertEqual(loaded.capture['id'], expected.capture['id'])
            self.assertEqual(source.read_bytes(), source_bytes)
            catalog = json.loads((root/'output.tree_instances.json').read_text())
            self.assertEqual(catalog['instances'], loaded[0].groups[0].tree_instances)
            roundtrip = root/'native-roundtrip.mtscene'
            native('--objects', output, roundtrip)
            saved = mtscene.load(roundtrip)
            self.assertEqual(retained_state(saved), retained_state(loaded))
            self.assertEqual(saved.route_data, loaded.route_data)
            self.assertEqual(saved.capture, loaded.capture)
            native('--remove-trees', roundtrip, root/'repeat.mtscene')
            self.assertEqual(retained_state(mtscene.load(root/'repeat.mtscene')), retained_state(loaded))
            blocked = subprocess.run([exe, '--remove-trees', str(source), str(output)], capture_output=True)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertEqual(retained_state(mtscene.load(output)), retained_state(loaded))


if __name__ == '__main__':
    unittest.main()

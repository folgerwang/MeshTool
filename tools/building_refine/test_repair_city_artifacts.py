import copy
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mtscene
import numpy as np
from compose_iteration import fingerprint
from iterate_building_shells import local_group
from repair_city_artifacts import active, run, remove_collapsed_triangles
import test_repair_stepped_building
from shapely.geometry import box


class CityArtifactRepairTest(unittest.TestCase):
    def fixture(self, root):
        source, _, _, _ = test_repair_stepped_building.SteppedBuildingRepairTest().fixture(root)
        manifest = dict(buildings=[dict(building='building_188', batch=0, group=0,
            object_id=0, catalog=str(root/'missing.json'), status='converted')])
        (root/'manifest.json').write_text(json.dumps(manifest))
        return source

    def test_collapsed_triangle_removal_preserves_valid_triangle_and_uv(self):
        vertices = np.array([[0., 0, 0], [1, 0, 0], [0, 1, 0], [0, 0, 0], [1, 0, 0], [2, 0, 0]], np.float32)
        uv = np.arange(12, dtype=np.float32).reshape(-1, 2)
        mesh = mtscene.Mesh(0, 0, np.zeros(3), vertices.copy(), uv.copy(), None,
                            [(4, np.arange(6, dtype=np.uint32))])
        self.assertEqual(remove_collapsed_triangles([mesh]), 1)
        np.testing.assert_array_equal(mesh.vertices, vertices)
        np.testing.assert_array_equal(mesh.uvs, uv)
        np.testing.assert_array_equal(mesh.draw_calls[0][1], [0, 1, 2])

    def test_unsupported_fit_retains_active_capture_neighbor_and_textures(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = self.fixture(root)
            before = mtscene.load(source)[0].groups[0]
            with patch('repair_city_artifacts.candidates', return_value=([], [])):
                run(source, root/'out')
            after = mtscene.load(root/'out/city.mtscene')[0].groups[0]
            self.assertEqual(fingerprint(before.meshes), fingerprint(after.meshes))
            self.assertEqual(before.textures, after.textures)
            report = json.loads((root/'out/repair_report.json').read_text())
            self.assertEqual(report['counts'], {'needs_measured_review': 1})

    def test_accepted_fit_installs_closed_openings_and_preserves_unselected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = self.fixture(root)
            before = mtscene.load(source)[0].groups[0]
            candidate = local_group(before, 0)
            layers = [(9, box(0, 0, 18, 15)), (24, box(3, 3, 12, 12))]
            with patch('repair_city_artifacts.candidates', return_value=(
                    [(2., copy.deepcopy(candidate), layers, dict(mode='fixture'))], [])):
                run(source, root/'out')
            after = mtscene.load(root/'out/city.mtscene')[0].groups[0]
            self.assertEqual(fingerprint([m for m in before.meshes if m.model_variant == 1]),
                             fingerprint([m for m in after.meshes if m.model_variant == 1]))
            self.assertEqual(fingerprint([m for m in before.meshes if m.object_id == 1]),
                             fingerprint([m for m in after.meshes if m.object_id == 1]))
            self.assertNotEqual(fingerprint(active(before, 0)), fingerprint(active(after, 0)))
            report = json.loads((root/'out/repair_report.json').read_text())
            self.assertEqual(report['counts'], {'repaired': 1})
            checks = report['buildings'][0]['validation']
            self.assertFalse(any(checks['audit'].values()))
            self.assertGreater(checks['filled_openings_checked'], 0)
            self.assertLessEqual(checks['max_unfilled_opening_m2'], .001)


if __name__ == '__main__':
    unittest.main()

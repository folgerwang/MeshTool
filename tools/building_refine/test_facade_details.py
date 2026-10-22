import copy
import json
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

import numpy as np
from shapely.geometry import box, Polygon

import mtscene
from building_rules import extrusion
from capture_tiles import refine_building
from compose_iteration import fingerprint
from facade_details import face_details, enhance
from solid_city import audit_shell, shell, atlas


class FacadeDetailsTest(unittest.TestCase):
    def test_float32_collapsed_trim_is_skipped_without_breaking_envelope(self):
        textures = [atlas((175, 175, 175)), atlas((155, 155, 155))]
        obj = mtscene.SceneObject('test_building', 3)
        with tempfile.TemporaryDirectory() as directory:
            meshes, _ = refine_building(shell(box(0, 0, 12, 10), 0, 12, 0),
                                        obj, textures, Path(directory), clean=True)
            catalog = json.loads((Path(directory)/'tiles.json').read_text())
        group = mtscene.Group(textures, meshes, [obj])
        thin = Polygon([(1, .5), (2, .5), (2, .50000000001)])
        with patch('facade_details.face_details', return_value=[('base_course', thin, .065)]):
            report = enhance(group, catalog)
        self.assertGreater(report['skipped_collapsed_trim'], 0)
        self.assertEqual(report['counts'], {})
        self.assertFalse(any(report['validation']['audit'].values()))
        self.assertFalse(any(i.get('kind') == 'facade_detail' for i in catalog['instances']))

    def test_details_are_closed_and_clear_of_openings_and_existing_signs(self):
        items = [dict(kind='window', bounds=[x, y, x+1, y+1.8])
                 for x in (1., 3., 5.) for y in (2., 5., 8.)]
        items += [dict(kind='door_candidate', bounds=[7, 0, 8.5, 2.5]),
                  dict(kind='wall_detail', bounds=[.5, 4.3, 2.5, 4.7])]
        wall = box(0, 0, 10, 12).difference(box(8, 6, 10, 12))
        details = face_details(wall, items, (10, 12))
        self.assertEqual({r for r, p, d in details},
                         {'window_surround', 'window_sill', 'floor_course', 'base_course', 'cornice'})
        solids = []
        for role, polygon, depth in details:
            self.assertTrue(wall.covers(polygon))
            for item in items:
                self.assertLess(polygon.intersection(box(*item['bounds'])).area, 1e-10)
            points = extrusion(polygon, -.025, depth)
            solids.append(mtscene.Mesh(0, 0, np.zeros(3), points, None, None,
                                      [(4, np.arange(len(points), dtype=np.uint32))]))
        self.assertFalse(any(audit_shell(solids).values()))
        for i, (_, a, _) in enumerate(details):
            for _, b, _ in details[i+1:]:
                self.assertLess(a.intersection(b).area, 1e-10)

    def test_blank_wall_gets_no_speculative_details(self):
        self.assertEqual(face_details(box(0, 0, 5, 8), [], (5, 8)), [])

    def test_near_corner_sill_is_clipped_to_wall(self):
        wall = box(0, 0, 4, 7)
        details = face_details(wall, [dict(kind='window', bounds=[.04, 2, 1, 4])], (4, 7))
        self.assertTrue(any(role == 'window_sill' for role, _, _ in details))
        self.assertTrue(all(wall.covers(poly) for _, poly, _ in details))

    def test_enhancement_preserves_shell_materials_and_window_slots(self):
        textures = [atlas((175, 175, 175)), atlas((155, 155, 155))]
        obj = mtscene.SceneObject('test_building', 3)
        with tempfile.TemporaryDirectory() as directory:
            meshes, _ = refine_building(shell(box(0, 0, 12, 10), 0, 12, 0),
                                        obj, textures, Path(directory), clean=True)
            catalog = json.loads((Path(directory)/'tiles.json').read_text())
        group = mtscene.Group(textures, meshes, [obj])
        before = fingerprint(meshes)
        old_mesh_count = len(meshes)
        old_textures = copy.deepcopy(textures)
        slots = copy.deepcopy([i for i in catalog['instances'] if i.get('kind') == 'window'])
        report = enhance(group, catalog)
        self.assertGreater(report['counts']['window_sill'], 0)
        self.assertEqual(before, fingerprint(group.meshes[:old_mesh_count]))
        self.assertEqual(old_textures, group.textures[:len(old_textures)])
        self.assertEqual(slots, [i for i in catalog['instances'] if i.get('kind') == 'window'])
        self.assertEqual(report['validation']['max_trim_opening_overlap_m2'], 0)
        self.assertFalse(any(audit_shell(group.meshes).values()))
        for mesh in group.meshes[old_mesh_count:]:
            points = mesh.vertices.astype(float).reshape(-1, 3, 3)
            volume = np.einsum('ij,ij->i', points[:, 0], np.cross(points[:, 1], points[:, 2])).sum()/6
            self.assertGreater(volume, 0, 'Trim surfaces must face outward')
            self.assertEqual(group.textures[mesh.tex_index].mips[0][:2], (4, 4))
        with self.assertRaisesRegex(ValueError, 'already detailed'):
            enhance(group, catalog)


if __name__ == '__main__':
    unittest.main()

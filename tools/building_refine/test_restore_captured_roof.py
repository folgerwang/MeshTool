import unittest
import numpy as np
from shapely.geometry import box
import mtscene
from restore_captured_roof import clip_mesh, boundary_length
from solid_city import shell, audit_shell


class RoofClipTest(unittest.TestCase):
    def mesh(self, points):
        points = np.asarray(points, np.float32)
        return mtscene.Mesh(0, 0, np.array([100., 200., 10.]), points,
            points[:, :2].copy(), None, [(4, np.arange(len(points), dtype=np.uint32))])

    def test_split_interpolates_uv_and_preserves_original_vertices(self):
        mesh = self.mesh([[0, 0, 0], [2, 0, 2], [0, 2, 0]])
        before = mesh.vertices.copy()
        low, high = clip_mesh(mesh, 11., False), clip_mesh(mesh, 11., True)
        np.testing.assert_array_equal(mesh.vertices, before)
        for clipped, above in [(low, False), (high, True)]:
            ids = clipped.draw_calls[0][1]
            z = clipped.vertices[ids, 2]+10
            self.assertTrue(np.all(z >= 11) if above else np.all(z <= 11))
            np.testing.assert_allclose(clipped.uvs, clipped.vertices[:, :2])
        def area(m):
            p = m.vertices[m.draw_calls[0][1]].reshape(-1, 3, 3)
            return np.linalg.norm(np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0]), axis=1).sum()/2
        self.assertAlmostEqual(float(area(low)+area(high)), float(area(mesh)), places=6)
        self.assertAlmostEqual(boundary_length([low, high]), boundary_length([mesh]), places=3)

    def test_complementary_clips_have_no_new_seam_edges(self):
        meshes = shell(box(0, 0, 10, 8), 0, 12, 0)
        pieces = [part for mesh in meshes for above in (False, True)
                  if (part := clip_mesh(mesh, 5.25, above)) is not None]
        self.assertFalse(any(audit_shell(pieces).values()))

    def test_coplanar_triangles_belong_to_roof_once(self):
        mesh = self.mesh([[0, 0, 1], [2, 0, 1], [0, 2, 1]])
        self.assertIsNone(clip_mesh(mesh, 11., False))
        self.assertIsNotNone(clip_mesh(mesh, 11., True))

    def test_cut_through_existing_vertex_does_not_emit_degenerate_triangle(self):
        mesh = self.mesh([[0, 0, 1], [2, 0, 2], [0, 2, 0]])
        for above in (False, True):
            part = clip_mesh(mesh, 11., above)
            self.assertEqual(len(part.draw_calls[0][1]), 3)
            self.assertEqual(audit_shell([part])['degenerate_triangles'], 0)


if __name__ == '__main__':
    unittest.main()

import copy
import unittest

import mtscene
from compose_iteration import fingerprint
from remove_trees import remove, retained_state
from test_pcg import fixture


class RemoveTreesTest(unittest.TestCase):
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


if __name__ == '__main__':
    unittest.main()

import unittest
import numpy as np
from shapely.geometry import box,Polygon
from solid_city import shell,audit_shell
from envelope_repair import repair,footprint


def upper(meshes):
    points=np.concatenate([(m.vertices[idx]+m.translation).reshape(-1,3,3)
        for m in meshes for primitive,idx in m.draw_calls])
    n=np.cross(points[:,1]-points[:,0],points[:,2]-points[:,0])
    return points[n[:,2]>0]


class EnvelopeRepairTest(unittest.TestCase):
    def test_capture_connects_fragmented_strips_beyond_normal_area_limit(self):
        own=shell(box(0,0,4,20),0,20,0)+shell(box(16,0,20,20),0,20,0)
        fixed,report=repair(own,upper(shell(box(0,0,20,20),0,20,0)),Polygon(),box(-1,-1,21,21))
        self.assertTrue(report['consolidated_parts'])
        self.assertEqual(report['parts_after'],1)
        self.assertAlmostEqual(footprint(fixed).area,400,places=2)
        self.assertFalse(any(audit_shell(fixed).values()))

    def test_disconnected_roofs_do_not_authorize_large_expansion(self):
        own=shell(box(0,0,4,20),0,20,0)+shell(box(26,0,30,20),0,20,0)
        roof=upper(shell(box(0,0,12,20),0,20,0)+shell(box(18,0,30,20),0,20,0))
        fixed,report=repair(own,roof,Polygon(),box(-1,-1,31,21))
        self.assertIs(fixed,own)
        self.assertEqual(report['status'],'excessive_outline_change')

    def test_roof_evidence_fills_scan_notch_without_entering_neighbor(self):
        complete=box(0,0,20,20)
        damaged=complete.difference(box(5,10,15,20))
        own=shell(damaged,0,20,0)
        fixed,report=repair(own,upper(shell(complete,0,20,0)),box(10,15,15,20),box(-1,-1,21,21))
        self.assertEqual(report['status'],'captured_roof_supported_envelope')
        self.assertGreater(footprint(fixed).area,footprint(own).area+50)
        self.assertLess(footprint(fixed).intersection(box(10,15,15,20)).area,.01)
        self.assertFalse(any(audit_shell(fixed).values()))

    def test_unobserved_courtyard_is_not_filled(self):
        poly=Polygon([(0,0),(30,0),(30,30),(0,30)],holes=[[(8,8),(8,22),(22,22),(22,8)]])
        own=shell(poly,0,20,0)
        fixed,_=repair(own,upper(own),Polygon(),box(-1,-1,31,31))
        self.assertLess(footprint(fixed).intersection(box(9,9,21,21)).area,.01)

    def test_no_capture_keeps_existing_closed_shell(self):
        own=shell(box(0,0,20,20),0,20,0)
        fixed,report=repair(own,np.empty((0,3,3)),Polygon(),box(-1,-1,21,21))
        self.assertIs(fixed,own)
        self.assertEqual(report['status'],'no_source_roof')


if __name__=='__main__':unittest.main()

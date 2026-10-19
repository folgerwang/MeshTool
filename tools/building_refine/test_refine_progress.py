import contextlib
import io
import shlex
import unittest
from refine_progress import Reporter


class ProgressTest(unittest.TestCase):
    def test_batch_percentage_does_not_track_candidate_percentage(self):
        calls=[];report=Reporter(lambda n,t,m:calls.append((n,t,m)),emit=True)
        stream=io.StringIO()
        with contextlib.redirect_stdout(stream):
            report.begin('building "one"',3)
            report.update(iteration=2,iterations=3,candidate=22,budget=40,mean=.1,worst=.15,threshold=.2,passed=9,views=9)
            self.assertEqual(calls[-1][0],0)
            report.finish(True)
            report.begin('building_two',3)
            self.assertEqual(report.candidate,0)
            self.assertEqual(report.worst,-1)
            report.finish(False)
            report.begin('building_three',3);report.finish(True)
        fractions=[n/t for n,t,_ in calls]
        self.assertEqual(fractions,sorted(fractions))
        self.assertEqual(fractions[-1],.95)
        self.assertEqual((report.completed,report.applied,report.kept),(3,2,1))
        lines=[shlex.split(s) for s in stream.getvalue().splitlines()]
        self.assertEqual(lines[1][14],'building "one"')
        self.assertEqual(lines[1][7],'22')

    def test_facade_counts_span_multiple_groups_and_skips(self):
        import facade
        from test_pcg import fixture
        report=Reporter()
        with contextlib.redirect_stdout(io.StringIO()):
            applied,total=facade.apply(fixture(),reporter=report)
        self.assertEqual((report.completed,report.total),(3,3))
        self.assertEqual(applied,report.applied)
        self.assertEqual(total,report.total)


if __name__=='__main__':unittest.main()

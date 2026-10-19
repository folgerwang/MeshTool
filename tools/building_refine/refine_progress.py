"""Batch progress and separate fit-quality telemetry for the native UI."""
import json


class Reporter:
    def __init__(self, callback=lambda *args: None, emit=False):
        self.callback, self.emit = callback, emit
        self.completed = self.total = self.applied = self.kept = 0
        self.name = '';self.phase = 'Loading scene'
        self.reset_fit()

    def reset_fit(self):
        self.iteration = self.iterations = self.candidate = self.budget = 0
        self.mean = self.worst = self.threshold = -1.
        self.passed = self.views = 0;self.worst_view = ''

    def begin(self, name, total, phase='Generating model'):
        self.name, self.total, self.phase = name, total, phase
        self.reset_fit();self.publish()

    def update(self, **fields):
        for key,value in fields.items(): setattr(self,key,value)
        self.publish()

    def finish(self, applied, reason=''):
        self.completed += 1
        self.applied += int(applied);self.kept += int(not applied)
        self.phase = reason or ('Applied' if applied else 'Unchanged')
        self.publish()

    def publish(self):
        # Reserve the final 5% for writing the paired scene. Search candidates
        # are a bounded budget, never an overall completion percentage.
        done = int(9500*self.completed/max(1,self.total))
        self.callback(done,10000,f'{self.name}: {self.phase}' if self.name else self.phase)
        if self.emit:
            numbers = (self.completed,self.total,self.applied,self.kept,self.iteration,self.iterations,
                       self.candidate,self.budget,self.mean,self.worst,self.threshold,self.passed,self.views)
            quote = lambda s: json.dumps(str(s).replace('\n',' ').replace('\r',' '),ensure_ascii=False)
            print('@refine '+' '.join(map(str,numbers))+' '+quote(self.name)+' '+quote(self.phase)+' '+quote(self.worst_view),flush=True)

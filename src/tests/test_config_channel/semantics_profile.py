# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
import gdb
import json
import os
import time

output = os.environ['FLOWSQL_SEMANTICS_PROFILE_OUTPUT']
sequence = 0
active = {}
audit_enabled = False
audit_symbols = [
    ('flowsql::channels::config::DataSemanticsRegistry::Resolve', 'definition_resolve'),
    ('flowsql::channels::config::DataSemanticsRegistry::ReadBody', 'definition_body'),
    ('flowsql::channels::config::DataSemanticsRegistry::Read', 'definition_read'),
    ('flowsql::channels::config::NormalizeSemantics', 'full_parse'),
]

def enable_audit():
    global audit_enabled
    if not audit_enabled and (os.environ.get('FLOWSQL_SEMANTICS_PROFILE_AUDIT') == '1' or os.path.exists(output + '.audit')):
        audit_enabled = True
        for symbol, stage in audit_symbols:
            Entered(symbol, stage)

def emit(value):
    with open(output, 'a') as stream:
        stream.write(json.dumps(value) + '\n')

class Finished(gdb.FinishBreakpoint):
    def __init__(self, frame, stage):
        super().__init__(frame, internal=True)
        global sequence
        sequence += 1
        self.stage = stage
        self.token = sequence
        self.thread = gdb.selected_thread().global_num
        self.parent = active.get(self.thread)
        self.start = time.perf_counter_ns()
        active[self.thread] = self.token
    def stop(self):
        emit(dict(stage=self.stage, token=self.token, parent=self.parent, thread=self.thread,
                  start_ns=self.start, end_ns=time.perf_counter_ns(), status='returned'))
        active[self.thread] = self.parent
        return False
    def out_of_scope(self):
        emit(dict(stage=self.stage, token=self.token, status='out_of_scope'))
        active[self.thread] = self.parent

class Entered(gdb.Breakpoint):
    def __init__(self, symbol, stage):
        super().__init__(symbol, internal=True)
        self.stage = stage
    def stop(self):
        if self.stage == 'task_open': enable_audit()
        Finished(gdb.newest_frame(), self.stage)
        return False

gdb.execute('set pagination off')
gdb.execute('set debuginfod enabled off')
gdb.execute('set confirm off')
gdb.execute('set breakpoint pending on')
gdb.execute('set disable-randomization off')
for symbol, stage in [
    ('flowsql::channels::config::OperatorContractService::CheckAndDerive', 'contract_check'),
    ('flowsql::channels::config::OperatorContractService::RecordUse', 'use_record'),
    ('flowsql::npm::PublishNpmResultSemantics', 'publication'),
    ('npm_result_semantics.cpp:Bind', 'object_publication'),
    ('flowsql::npm::NpmBasicTask::Open', 'task_open'),
    ('flowsql::npm::NpmBasicTask::ProcessBlock', 'data_process'),
    ('flowsql::npm::NpmBasicTask::Flush', 'data_flush'),
    ('flowsql::npm::NpmBasicTask::OnTime', 'data_time'),
]:
    Entered(symbol, stage)
enable_audit()
gdb.execute('run')

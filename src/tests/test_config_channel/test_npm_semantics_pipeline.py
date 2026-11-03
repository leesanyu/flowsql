# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Finite real PCAP -> production scheduler/NPM -> SQLite -> ordinary metadata client."""
import base64
import contextlib
import ipaddress
import json
import os
from pathlib import Path
import queue
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading

import yaml
from test_data_semantics_api import ROOT, validate


def dns(identifier, flags=0):
    return struct.pack('!6H', identifier, flags, 1, 0, 0, 0) + b'\x01a\0' + struct.pack('!HH', 1, 1)


def packet(family, body, port, response=False, tcp=False, seq=101, ack=201, flags=0x18, destination_port=53):
    source = '192.0.2.1' if family == 4 else '2001:db8::1'
    target = '192.0.2.2' if family == 4 else '2001:db8::2'
    sport, dport = port, destination_port
    if response:
        source, target, sport, dport = target, source, dport, sport
    if tcp:
        transport = struct.pack('!HHIIBBHHH', sport, dport, seq, ack, 0x50, flags, 65535, 0, 0) + body
    else:
        transport = struct.pack('!HHHH', sport, dport, 8 + len(body), 0) + body
    protocol = 6 if tcp else 17
    if family == 4:
        network = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(transport), 0, 0, 64, protocol, 0,
                              ipaddress.ip_address(source).packed, ipaddress.ip_address(target).packed)
    else:
        network = struct.pack('!IHBB16s16s', 6 << 28, len(transport), protocol, 64,
                              ipaddress.ip_address(source).packed, ipaddress.ip_address(target).packed)
    return b'\x02\0\0\0\0\x02\x02\0\0\0\0\x01' + struct.pack('!H', 0x800 if family == 4 else 0x86dd) + network + transport


def write_dns_capture(path):
    records = []
    for family in (4, 6):
        base = family * 1_000_000
        def send(offset, identifier, flags=0, port=40001, response=False, clipped=False):
            body = packet(family, dns(identifier, flags), port, response)
            if clipped:
                body = bytearray(body)
                for position in ((16, 38) if family == 4 else (18, 58)):
                    length = struct.unpack_from('!H', body, position)[0]
                    struct.pack_into('!H', body, position, length + 8)
            records.append((base + offset, body, len(body) + (8 if clipped else 0)))
        send(0, 1)
        send(1000, 1)  # repeated question, first-query time retained
        send(10000, 1, 0x8003, response=True)
        send(20000, 2, port=40002)  # no response before EOF
        send(30000, 3, 0x8000, port=40003, response=True)
        send(40000, 4, port=40004)
        send(50000, 4, 0x8200, port=40004, response=True)
        send(60000, 5, 0x0800, port=40005)  # unsupported opcode
        send(70000, 6, port=40006)
        send(80000, 6, 0x8000, port=40006, response=True, clipped=True)
        send(100000, 7, port=40007)
        send(99000, 7, 0x8000, port=40007, response=True)  # timestamp regression, not a latency sample
        query, response = dns(8), dns(8, 0x8000)
        for offset, body, reverse, seq, ack, flags in [
            (110000, b'', False, 100, 0, 2), (111000, b'', True, 200, 101, 0x12),
            (112000, b'', False, 101, 201, 0x10),
            (113000, struct.pack('!H', len(query)) + query, False, 101, 201, 0x18),
            (123000, struct.pack('!H', len(response)) + response, True, 201, 101 + len(query) + 2, 0x18),
        ]:
            body = packet(family, body, 40008, reverse, True, seq, ack, flags)
            records.append((base + offset, body, len(body)))
    with path.open('wb') as output:
        output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for microseconds, body, wire_length in records:
            seconds, micros = divmod(microseconds, 1_000_000)
            output.write(struct.pack('<IIII', seconds, micros, len(body), wire_length) + body)


def write_session_capture(path):
    records = []
    for family in (4, 6):
        def send(micros, port, body=b'', reverse=False, tcp=False, seq=101, ack=201, flags=0x18):
            raw = packet(family, body, port, reverse, tcp, seq, ack, flags)
            records.append((micros, raw))
        for t in [1_100_000, 2_100_000, 3_100_000]:
            send(t, 41001, b'udp')
        send(4_000_000, 41999, b'tail')
        for t, body, reverse, seq, ack, flags in [
            (1_100_000, b'', False, 100, 0, 2), (1_200_000, b'', True, 200, 101, 0x12),
            (1_300_000, b'', False, 101, 201, 0x10), (1_400_000, b'data', False, 101, 201, 0x18),
            (1_500_000, b'data', False, 101, 201, 0x18), (1_600_000, b'', True, 201, 105, 0x10),
            (1_700_000, b'more', False, 105, 201, 0x18), (1_800_000, b'', True, 201, 109, 0x10),
        ]:
            send(t, 41002, body, reverse, True, seq, ack, flags)
        send(1_100_000, 41003, b'x', tcp=True)  # midstream, no ACK, zero span
        send(1_100_000, 41004, b'x', tcp=True, seq=100)
        send(1_200_000, 41004, b'y', tcp=True, seq=101 + 2**31)  # sequence ambiguity
        send(1_100_000, 41005, tcp=True, seq=100, ack=0, flags=2)  # partial handshake
    with path.open('wb') as output:
        output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for micros, raw in sorted(records, key=lambda item: item[0]):
            seconds, fraction = divmod(micros, 1_000_000)
            output.write(struct.pack('<IIII', seconds, fraction, len(raw), len(raw)) + raw)


def run_sessions(call, mode):
    config = dict(schema_version=1, core=dict(result_mode=mode, output_interval_ns=1_000_000_000,
                                            out_of_order_tolerance_ns=0))
    sql = ("SELECT * FROM pcapfile.fixture USING npm.basic WITH features='basic,session', "
           "observing='basic_ipv4', parameters='" + json.dumps(config, separators=(',', ':')) +
           "' INTO sqlite.example")
    return call('/scheduler/batch/execute', dict(sql=sql))


def check_sessions(call, root, result, mode):
    assert result['status'] == 'completed', result
    used = resolve(call, result['contract_checks'][0]['use_ref'])
    context = used['execution_context']['npm']
    entries = {item['entity_id']: item for item in context['entities']}
    assert set(entries) == {'basic_ipv4', 'basic_ipv6', 'session_ipv4', 'session_ipv6'}
    with sqlite3.connect(root / 'input.db') as database:
        database.row_factory = sqlite3.Row
        for family in (4, 6):
            for module, count in [('basic', 34), ('session', 52)]:
                entry = entries[f'{module}_ipv{family}']
                views = {}
                for relation in entry['relations']:
                    obj = describe(call, relation['locator'])
                    dictionary = obj['description']['document']
                    assert obj['basis']['dictionary_ref'] == relation['dictionary_ref']
                    assert len(dictionary['fields']) == count
                    assert dictionary['entity']['identity_fields']['value'] == ['__npm_run_id', 'session_id']
                    assert all(f['definition']['state'] == 'declared' for f in dictionary['fields'])
                    links = dictionary['entity']['relations']['value']
                    assert len(links) == (2 if module == 'session' else 1)
                    if module == 'session':
                        target = next(r for r in entries[f'basic_ipv{family}']['relations'] if r['view'] == relation['view'])
                        assert links[1]['dictionary_ref'] == target['dictionary_ref']
                        assert links[1]['field_map'] == {k: k for k in ['__npm_run_id', 'session_id', 'observation_domain_id']}
                    views[relation['view']] = [dict(row) for row in database.execute(
                        'SELECT * FROM "' + relation['locator']['table'] + '" WHERE __npm_run_id=? ORDER BY CAST(revision AS INTEGER)',
                        (context['run_id'],))]
                assert all(r['primary_label_id'] is None and r['protocol_status'] != 'pending' for r in views['final'])
                assert len(views['latest']) == len(views['final']) == 6
                assert len({row['session_id'] for row in views['history']}) == 6
                if module == 'basic':
                    history = [r for r in views['history'] if r['a_port'] == 41001]
                    final = next(r for r in views['final'] if r['a_port'] == 41001)
                    assert final['packets_ab'] == '3' and final['wire_bytes_total'] == str(3 * (45 if family == 4 else 65))
                    period = ['period_start_ns', 'period_end_ns', 'period_complete', 'interval_packets_ab',
                              'interval_packets_ba', 'interval_wire_bytes_ab', 'interval_wire_bytes_ba', 'interval_wire_bytes_total']
                    if mode == 'final':
                        assert len(history) == 1 and all(r[k] is None for r in views['history'] for k in period)
                    else:
                        assert [r['packets_ab'] for r in history] == ['1', '2', '3', '3'], history
                        assert [r['interval_packets_ab'] for r in history] == ['1', '1', '1', '0']
                        assert [r['period_start_ns'] for r in history] == [1_000_000_000, 2_000_000_000, 3_000_000_000, 3_000_000_000]
                        assert all(r['period_complete'] == 1 for r in history)
                        assert history[0]['is_final'] == 0 and history[-1]['is_final'] == 1
                        tail = next(r for r in views['final'] if r['a_port'] == 41999)
                        assert tail['period_complete'] == 0 and tail['is_final'] == 1
                        assert sum(int(r['interval_packets_ab']) for r in history) == 3
                        assert sum(int(r['packets_ab']) for r in history) == 9  # wrong cumulative sum demonstrably differs
                else:
                    rows = {r['a_port']: r for r in views['final']}
                    good = rows[41002]
                    assert good['tcp_handshake_status'] == 'complete' and good['tcp_initiator'] == 'a'
                    assert good['tcp_handshake_duration_ns'] == 200_000_000 and good['tcp_synack_rtt_ns'] == 100_000_000
                    assert good['duration_ns'] == 700_000_000 and good['rate_status'] == 'valid'
                    assert abs(good['wire_bps_ab'] - int(good['wire_bytes_ab']) * 8e9 / good['duration_ns']) < 0.001
                    assert good['tcp_retransmission_status'] == 'valid' and good['tcp_retrans_packets_ab'] == '1'
                    assert good['tcp_retrans_payload_bytes_ab'] == '4' and good['tcp_unique_payload_bytes_ab'] == '8'
                    assert good['tcp_rtt_status'] == 'valid' and int(good['tcp_rtt_samples']) >= 1
                    assert good['tcp_rtt_min_ns'] == good['tcp_rtt_mean_ns'] == good['tcp_rtt_max_ns'] == 100_000_000
                    empty = rows[41003]
                    assert empty['tcp_rtt_status'] == 'no_sample' and empty['tcp_rtt_samples'] == '0'
                    assert empty['tcp_rtt_mean_ns'] is None and empty['wire_bps_ab'] is None
                    assert empty['rate_status'] == 'insufficient_span' and empty['measurement_flags'] & 1
                    ambiguous = rows[41004]
                    assert ambiguous['tcp_rtt_status'] == ambiguous['tcp_retransmission_status'] == 'ambiguous', ambiguous
                    assert ambiguous['tcp_rtt_samples'] is None and ambiguous['tcp_unique_payload_bytes_ab'] is None
                    assert ambiguous['measurement_flags'] & 8
                    assert rows[41005]['tcp_handshake_status'] == 'partial' and rows[41005]['tcp_handshake_duration_ns'] is None
                    udp = rows[41001]
                    for key in ['tcp_handshake_status', 'tcp_rtt_status', 'tcp_retransmission_status']:
                        assert udp[key] == 'not_applicable'
                    assert udp['tcp_initiator'] is None and udp['tcp_rtt_samples'] is None
                    assert udp['tcp_retrans_packets_ab'] is None
    return context


def session_main(executable):
    evidence = ROOT / 'evidence/npm-data-semantics/t3-session'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-npm-session-') as temporary:
        root = Path(temporary)
        (root / 'frames').mkdir()
        (root / 'frames/lazy.csv').write_text('latency\n1000\n')
        write_session_capture(root / 'npm.pcap')
        with host_process(executable, root) as call:
            results = []
            for mode in ['periodic_snapshot', 'final']:
                result = run_sessions(call, mode)
                results.append(result)
                check_sessions(call, root, result, mode)
            assert results[0]['result']['run_id'] != results[1]['result']['run_id']
            (evidence / 'runs.json').write_text(json.dumps(results, ensure_ascii=False, indent=2))
        (evidence / 'execution.log').write_text((root / 'host.log').read_text())
    print('PASS: dual-family Basic periods/final NULL, Session performance states and exact same-run relations')


def tls_hello(client):
    def ext(code, value):
        return struct.pack('!HH', code, len(value)) + value
    body = b'\x03\x03' + (b'c' if client else b's') * 32 + b'\0'
    if client:
        body += b'\0\x02\x13\x01\x01\0'
        name = b'example.com'
        sni = b'\0' + struct.pack('!H', len(name)) + name
        extensions = ext(0, struct.pack('!H', len(sni)) + sni)
        extensions += ext(43, b'\x02\x03\x04')
        extensions += ext(16, b'\0\x03\x02h2')
    else:
        body += b'\x13\x01\0'
        extensions = ext(43, b'\x03\x04')
    body += struct.pack('!H', len(extensions)) + extensions
    handshake = bytes([1 if client else 2]) + len(body).to_bytes(3, 'big') + body
    return b'\x16\x03\x03' + struct.pack('!H', len(handshake)) + handshake


def icmp_packet(family, body, reverse=False):
    source = ipaddress.ip_address('192.0.2.1' if family == 4 else '2001:db8::1').packed
    target = ipaddress.ip_address('192.0.2.2' if family == 4 else '2001:db8::2').packed
    if reverse:
        source, target = target, source
    if family == 4:
        network = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(body), 0, 0, 64, 1, 0, source, target)
    else:
        network = struct.pack('!IHBB16s16s', 6 << 28, len(body), 58, 64, source, target)
    return b'\x02\0\0\0\0\x02\x02\0\0\0\0\x01' + struct.pack('!H', 0x800 if family == 4 else 0x86dd) + network + body


def write_protocol_capture(path):
    records = []
    for family in (4, 6):
        base = family * 1_000_000
        def tcp(port, target, request, response=None):
            for offset, data, reverse, seq, ack, flags in [
                (0, b'', False, 100, 0, 2), (1000, b'', True, 200, 101, 0x12),
                (2000, b'', False, 101, 201, 0x10), (3000, request, False, 101, 201, 0x18),
            ] + ([(13000, response, True, 201, 101 + len(request), 0x18)] if response else []):
                records.append((base + offset, packet(family, data, port, reverse, True, seq, ack, flags, target)))
        tcp(42001, 80, b'GET /missing HTTP/1.1\r\nHost: example.com\r\n\r\n',
            b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n')
        tcp(42002, 80, b'GET /pending HTTP/1.1\r\nHost: example.com\r\n\r\n')
        tcp(42003, 443, tls_hello(True), tls_hello(False))
        tcp(42004, 443, tls_hello(True))
        quote = packet(family, b'quoted', 42011)
        records.append((base + 100000, quote))
        for offset, request, identifier in [(200000, True, 1), (201000, True, 1), (210000, False, 1),
                                             (220000, True, 2), (230000, False, 3)]:
            kind = (8 if request else 0) if family == 4 else (128 if request else 129)
            body = struct.pack('!BBHHH', kind, 0, 0, identifier, 1) + b'echo'
            records.append((base + offset, icmp_packet(family, body, not request)))
        for offset, quoted in [(240000, quote[14:]), (250000, b'')]:
            body = struct.pack('!BBHI', 3 if family == 4 else 2, 4 if family == 4 else 0, 0, 1400) + quoted
            records.append((base + offset, icmp_packet(family, body, True)))
    with path.open('wb') as output:
        output.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
        for micros, raw in sorted(records, key=lambda item: item[0]):
            seconds, fraction = divmod(micros, 1_000_000)
            output.write(struct.pack('<IIII', seconds, fraction, len(raw), len(raw)) + raw)


def run_protocols(call, reference):
    config = dict(schema_version=1, core=dict(labeling=reference), http1=dict(primary_label_ids=[1003]),
                  tls=dict(primary_label_ids=[1004]))
    sql = ("SELECT * FROM pcapfile.fixture USING npm.basic WITH features='basic,session,labeling,http1,tls,icmp', "
           "observing='icmp_event_ipv4', parameters='" + json.dumps(config, separators=(',', ':')) +
           "' INTO sqlite.example")
    return call('/scheduler/batch/execute', dict(sql=sql))


def check_protocols(call, root, result):
    assert result['status'] == 'completed', result
    used = resolve(call, result['contract_checks'][0]['use_ref'])
    context = used['execution_context']['npm']
    entries = {item['entity_id']: item for item in context['entities']}
    with sqlite3.connect(root / 'input.db') as database:
        database.row_factory = sqlite3.Row
        for family in (4, 6):
            for module, entity_name, count in [('http1', 'http1_transaction', 25), ('tls', 'tls_handshake', 28),
                                               ('icmp', 'icmp_event', 30)]:
                entry = entries[f'{entity_name}_ipv{family}']
                event_sets = []
                for relation in entry['relations']:
                    obj = describe(call, relation['locator'])
                    dictionary = obj['description']['document']
                    assert obj['basis']['dictionary_ref'] == relation['dictionary_ref']
                    assert dictionary == resolve(call, relation['dictionary_ref'])
                    assert len(dictionary['fields']) == count and all(f['definition']['state'] == 'declared' for f in dictionary['fields'])
                    links = dictionary['entity']['relations']['value']
                    assert len(links) == (1 if module == 'icmp' else 2)
                    if module != 'icmp':
                        target = next(r for r in entries[f'session_ipv{family}']['relations'] if r['view'] == relation['view'])
                        assert links[1]['dictionary_ref'] == target['dictionary_ref']
                    rows = [dict(r) for r in database.execute('SELECT * FROM "' + relation['locator']['table'] +
                            '" WHERE __npm_run_id=?', (context['run_id'],))]
                    assert all(r['revision'] == '1' and r['is_final'] == 1 for r in rows)
                    event_sets.append({r['entity_instance_id'] for r in rows})
                    if module == 'http1':
                        assert len(rows) == 2, rows
                        matched = next(r for r in rows if r['outcome'] == 'matched')
                        assert matched['status_code'] == 404 and matched['latency_ns'] == 10_000_000
                        assert matched['request_direction'] == 0 and matched['response_direction'] == 1
                        assert matched['incomplete_reason'] is None and matched['method'] == 'GET'
                        single = next(r for r in rows if r['outcome'] == 'request_only')
                        assert single['status_code'] is None and single['latency_ns'] is None
                        assert single['response_headers_at_ns'] is None and single['response_direction'] is None
                        assert single['incomplete_reason'] == 'session_end'
                    elif module == 'tls':
                        assert len(rows) == 2, rows
                        observed = next(r for r in rows if r['outcome'] == 'server_hello_observed')
                        assert observed['selected_version'] == 0x304 and observed['cipher_suite'] == 0x1301
                        assert observed['server_hello_latency_ns'] == 10_000_000
                        assert observed['client_sni'] == 'example.com' and json.loads(observed['client_alpn_protocols']) == ['h2']
                        assert observed['client_offered_tls13'] == 1 and observed['selected_alpn'] is None
                        assert observed['incomplete_reason'] == 'encrypted_after_server_hello'
                        incomplete = next(r for r in rows if r['outcome'] == 'incomplete')
                        assert incomplete['server_hello_at_ns'] is None and incomplete['server_hello_latency_ns'] is None
                        assert incomplete['selected_version'] is None and incomplete['incomplete_reason'] == 'session_end'
                    else:
                        assert len(rows) == 5, rows
                        matched = next(r for r in rows if r['outcome'] == 'echo_matched')
                        assert matched['echo_retries'] == 1 and matched['latency_ns'] == 10_000_000
                        assert matched['icmp_type'] == (8 if family == 4 else 128)
                        request = next(r for r in rows if r['outcome'] == 'echo_request_only')
                        assert request['reply_at_ns'] is None and request['incomplete_reason'] == 'task_eof'
                        reply = next(r for r in rows if r['outcome'] == 'echo_reply_only')
                        assert reply['request_at_ns'] is None and reply['echo_retries'] is None
                        errors = [r for r in rows if r['outcome'] == 'icmp_error']
                        assert len(errors) == 2 and all(r['next_hop_mtu'] == 1400 for r in errors)
                        quoted = next(r for r in errors if r['quote_status'] == 'tuple')
                        assert quoted['quoted_src_port'] == 42011 and quoted['quoted_dst_port'] == 53
                        assert quoted['quoted_ip_family'] == family and quoted['active_quoted_session_id'] is not None
                        empty = next(r for r in errors if r['quote_status'] == 'unavailable')
                        assert empty['quoted_protocol'] is None and empty['quoted_src_port'] is None
                        assert empty['active_quoted_session_id'] is None and empty['latency_ns'] is None
                assert event_sets[0] == event_sets[1] == event_sets[2]
    return context


def protocol_main(executable):
    evidence = ROOT / 'evidence/npm-data-semantics/t4-protocols'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-npm-protocol-') as temporary:
        root = Path(temporary)
        (root / 'frames').mkdir()
        (root / 'frames/lazy.csv').write_text('latency\n1000\n')
        write_protocol_capture(root / 'npm.pcap')
        with host_process(executable, root) as call:
            labels = publish_labels(call, ['http1', 'tls'])
            result = run_protocols(call, labels)
            check_protocols(call, root, result)
            (evidence / 'run.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
        (evidence / 'execution.log').write_text((root / 'host.log').read_text())
    print('PASS: dual-family HTTP 404/single, TLS ServerHello/incomplete, ICMP Echo/errors/missing quote and exact relations')


@contextlib.contextmanager
def host_process(executable, root):
    messages = queue.Queue()
    log_path = root / 'host.log'
    process = subprocess.Popen(['stdbuf', '-oL', '-eL', str(executable), str(root), '0', '0', '0', '0', '--npm'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, cwd=executable.parent, env=dict(os.environ, FLOWSQL_GATEWAY_ADDR=''))
    def read():
        with log_path.open('w') as log:
            for line in process.stdout:
                log.write(line)
                log.flush()
                if line.startswith(('CONTRACT_HOST_READY', 'CONTRACT_LOCAL ')):
                    messages.put(line)
        messages.put(None)
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    def receive():
        try:
            value = messages.get(timeout=30)
        except queue.Empty:
            raise AssertionError(log_path.read_text())
        assert value, log_path.read_text()
        return value
    def call(path, payload, expected=0):
        process.stdin.write(json.dumps(dict(path=path, payload=payload)) + '\n')
        process.stdin.flush()
        line = receive()
        assert line.startswith('CONTRACT_LOCAL '), line
        result = json.loads(line.removeprefix('CONTRACT_LOCAL '))
        assert result['rc'] == expected, (path, result, log_path.read_text()[-6000:])
        return result['result']
    try:
        assert receive().startswith('CONTRACT_HOST_READY')
        yield call
    finally:
        if process.poll() is None:
            try:
                process.stdin.write('quit\n')
                process.stdin.flush()
            except BrokenPipeError:
                pass  # The primary failure includes the host log below.
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError('NPM fixture did not stop: ' + log_path.read_text()[-4000:])
        reader.join(timeout=2)
        assert process.returncode == 0, log_path.read_text()[-6000:]


def semantic(call, operation, payload, expected=0):
    key = operation.replace('/', '_') if operation.startswith('channels/') else operation
    if expected == 0:
        validate(key + 'Request', payload)
    result = call('/semantics/v1/' + operation, payload, expected)
    validate(key + 'Response' if expected == 0 else 'error', result)
    return result.get('data', result.get('error'))


def resolve(call, reference):
    return semantic(call, 'resolve', dict(exact_reference=reference, view='document'))['document']


def describe(call, locator):
    return semantic(call, 'describe', dict(target=dict(kind='channel', locator=locator)))


def publish_labels(call, protocols=("dns",)):
    labels = yaml.safe_load((ROOT / 'config/flow-labeling-template.yaml').read_text())
    labels['metadata']['name'] = 'npm-semantics-fixture'
    definitions = {'dns': (1002, 53), 'http1': (1003, 80), 'tls': (1004, 443)}
    labels['spec']['labels'] = [dict(id=definitions[name][0], priority=2000 + i, name=name,
                                    display_name=name.upper(), description='Fixture ' + name)
                               for i, name in enumerate(protocols)]
    labels['spec']['rules'] = [dict(id=name, label_id=definitions[name][0], direction='bidirectional',
                                   matches=[dict(field='destination_port', min=definitions[name][1],
                                                 max=definitions[name][1])]) for name in protocols]
    content = yaml.safe_dump(labels).encode()
    result = call('/channels/config/publish', dict(name='npm-labels', expected_current_revision=0, format='yaml',
                  schema_id='flowsql.io/flow-labeling/v1alpha1', content_base64=base64.b64encode(content).decode()))
    return result['exact_reference']


def run_dns(call, reference):
    config = dict(schema_version=1, core=dict(labeling=reference), dns=dict(primary_label_ids=[1002]))
    parameters = json.dumps(config, separators=(',', ':'))
    sql = ("SELECT * FROM pcapfile.fixture USING npm.basic WITH features='basic,session,labeling,dns', "
           "observing='dns_transaction_ipv4', parameters='" + parameters + "' INTO sqlite.example")
    return call('/scheduler/batch/execute', dict(sql=sql))


def assert_dns_rows(rows):
    by_id = {row['dns_id']: row for row in rows}
    assert set(by_id) == set(range(1, 9)), rows
    assert {row['outcome'] for row in rows} == {'matched', 'query_only', 'response_only',
                                               'truncated_response', 'unsupported_message'}
    assert by_id[1]['outcome'] == 'matched' and by_id[1]['response_rcode'] == 3
    assert by_id[1]['query_retries'] == 1 and by_id[1]['latency_ns'] == 10_000_000
    assert by_id[1]['query_direction'] == 0 and by_id[1]['response_direction'] == 1
    assert by_id[2]['outcome'] == 'query_only' and by_id[2]['incomplete_reason'] == 'session_end'
    assert by_id[2]['response_at_ns'] is None and by_id[2]['response_rcode'] is None
    assert by_id[3]['outcome'] == 'response_only' and by_id[3]['query_at_ns'] is None
    assert by_id[3]['query_direction'] is None and by_id[3]['latency_ns'] is None
    assert by_id[4]['outcome'] == 'truncated_response' and by_id[4]['response_tc'] == 1
    assert by_id[4]['incomplete_reason'] == 'dns_tc_set' and by_id[4]['latency_ns'] == 10_000_000
    assert by_id[5]['outcome'] == 'unsupported_message'
    assert by_id[6]['outcome'] == 'query_only' and by_id[6]['incomplete_reason'] == 'capture_truncation'
    assert by_id[6]['response_tc'] is None and by_id[6]['response_at_ns'] is None
    assert by_id[7]['outcome'] == 'matched' and by_id[7]['latency_ns'] is None
    assert by_id[8]['transport_protocol'] == 6 and by_id[8]['outcome'] == 'matched'
    assert by_id[8]['latency_ns'] == 10_000_000
    assert all(row['is_final'] == 1 and row['revision'] == '1' for row in rows)


def check_run(call, root, result):
    assert result['status'] == 'completed', result
    managed = result['result']
    use_ref = result['contract_checks'][0]['use_ref']
    used = resolve(call, use_ref)
    context = used['execution_context']['npm']
    assert context['run_id'] == managed['run_id'] and context['run_id'] != used['run_id']
    assert context['input_namespace'] == 'pcapfile.fixture'
    assert used['normalized_config']['core']['run_mode'] == 'offline'
    assert used['execution_context']['label_selection_state'] == 'checked'
    opaque = next(item for item in used['outputs'] if item['port_id'] == 'managed_results')
    assert opaque['state'] == 'not_observed', opaque
    descriptions = {}
    with sqlite3.connect(root / 'input.db') as database:
        database.row_factory = sqlite3.Row
        for entity in context['entities']:
            if not entity['entity_id'].startswith('dns_'):
                continue
            assert entity['description_state'] == 'published' and len(entity['relations']) == 3
            all_ids = []
            for relation in entity['relations']:
                locator = relation['locator']
                obj = describe(call, locator)
                assert obj['basis']['dictionary_ref'] == relation['dictionary_ref']
                dictionary = obj['description']['document']
                assert dictionary == resolve(call, relation['dictionary_ref'])
                fields = {field['name']: field for field in dictionary['fields']}
                assert len(fields) == 28 and fields['latency_ns']['unit']['value']['code'] == 'ns'
                session = next(e for e in context['entities'] if e['entity_id'] == entity['entity_id'].replace('dns_transaction', 'session'))
                target = next(r for r in session['relations'] if r['view'] == relation['view'])
                links = dictionary['entity']['relations']['value']
                assert len(links) == 2 and links[1]['dictionary_ref'] == target['dictionary_ref']
                assert links[1]['field_map'] == {k: k for k in ['__npm_run_id', 'session_id', 'observation_domain_id']}
                assert 'RCODE' in fields['response_rcode']['definition']['value']
                assert fields['__npm_run_status']['definition']['value'].find('completed') >= 0
                assert all(field['type']['system'] == 'database' for field in fields.values())
                descriptions[locator['table']] = relation
                rows = [dict(row) for row in database.execute(
                    'SELECT * FROM "' + locator['table'] + '" WHERE __npm_run_id=?', (managed['run_id'],))]
                assert_dns_rows(rows)
                all_ids.append({row['entity_instance_id'] for row in rows})
            assert all_ids[0] == all_ids[1] == all_ids[2]  # one event, three views
    # Read the actual object through the existing product query entry as well.
    query = call('/scheduler/batch/execute', dict(sql='SELECT * FROM sqlite.example.npm_dns_transaction_ipv4_final_v1'))
    assert query['rows'] >= 8, query
    return context, descriptions


def main():
    executable = Path(sys.argv[1]).resolve()
    if len(sys.argv) > 2 and sys.argv[2] == "--protocols":
        return protocol_main(executable)
    if len(sys.argv) > 2 and sys.argv[2] == "--sessions":
        return session_main(executable)
    evidence = ROOT / 'evidence/npm-data-semantics/t2-dns-pipeline'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='flowsql-npm-domain-') as temporary:
        root = Path(temporary)
        (root / 'frames').mkdir()
        (root / 'frames/lazy.csv').write_text('latency\n1000\n')
        write_dns_capture(root / 'npm.pcap')
        with host_process(executable, root) as call:
            labels = publish_labels(call)
            result = run_dns(call, labels)
            context, descriptions = check_run(call, root, result)
            before = semantic(call, 'list', {})
            for relation in descriptions.values():
                describe(call, relation['locator'])
            assert semantic(call, 'list', {}) == before
            second = run_dns(call, labels)
            next_context, _ = check_run(call, root, second)
            assert next_context['run_id'] != context['run_id']
            assert next_context['entities'] == context['entities']
            (evidence / 'runs.json').write_text(json.dumps([result, second], ensure_ascii=False, indent=2))
        (evidence / 'execution.log').write_text((root / 'host.log').read_text())
        with host_process(executable, root) as call:
            for relation in descriptions.values():
                reloaded = describe(call, relation['locator'])
                assert reloaded['basis']['dictionary_ref'] == relation['dictionary_ref']
                assert resolve(call, relation['dictionary_ref']) == reloaded['description']['document']
        (evidence / 'restart.log').write_text((root / 'host.log').read_text())
    print('PASS: finite dual-family UDP/TCP DNS, five outcomes, real metadata, exact run/use refs, views and restart')


if __name__ == '__main__':
    main()

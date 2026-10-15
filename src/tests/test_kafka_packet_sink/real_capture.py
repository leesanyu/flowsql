# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Run an owned plaintext broker and real NIC/NPM paths in one private network namespace."""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools'))
from test_environment.config import Config, EnvironmentError, STATE
from test_environment.fixture import kafka_distribution, private_write
from test_environment.runner import scrub


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--inside', action='store_true')
    parser.add_argument('--config')
    args = parser.parse_args()
    config = Config(args.config)
    if not args.inside:
        command = ['unshare', '--net', sys.executable, str(Path(__file__).resolve()), '--inside',
                   '--config', str(config.path)]
        return subprocess.run(command).returncode
    if os.readlink('/proc/self/ns/net') == os.readlink('/proc/1/ns/net'):
        raise EnvironmentError('execution_denied', 'private_namespace_required')
    for command in (['sysctl', '-q', '-w', 'net.ipv6.conf.all.disable_ipv6=1'],
                    ['sysctl', '-q', '-w', 'net.ipv6.conf.default.disable_ipv6=1'],
                    ['ip', 'link', 'set', 'lo', 'up'],
                    ['ip', 'link', 'add', 'ps0', 'type', 'veth', 'peer', 'name', 'pt0'],
                    ['ip', 'link', 'set', 'ps0', 'up'], ['ip', 'link', 'set', 'pt0', 'up']):
        subprocess.run(command, check=True, capture_output=True, timeout=5)
    root = STATE / config.data['project'] / ('packet-sink-e2e-' + uuid.uuid4().hex)
    root.mkdir(parents=True, mode=0o700)
    distribution = kafka_distribution(config)
    ports = []
    while len(ports) < 2:
        with socket.socket() as endpoint:
            endpoint.bind(('127.0.0.1', 0))
            port = endpoint.getsockname()[1]
        if port not in ports:
            ports.append(port)
    port, controller = ports
    properties = root / 'broker.properties'
    private_write(properties, '\n'.join([
        'process.roles=broker,controller', 'node.id=1', 'controller.listener.names=CONTROLLER',
        f'controller.quorum.voters=1@localhost:{controller}',
        f'listeners=PLAINTEXT://127.0.0.1:{port},CONTROLLER://127.0.0.1:{controller}',
        f'advertised.listeners=PLAINTEXT://localhost:{port}',
        'listener.security.protocol.map=PLAINTEXT:PLAINTEXT,CONTROLLER:PLAINTEXT',
        'inter.broker.listener.name=PLAINTEXT', 'auto.create.topics.enable=false',
        'offsets.topic.replication.factor=1', 'transaction.state.log.replication.factor=1',
        'transaction.state.log.min.isr=1', 'group.initial.rebalance.delay.ms=0',
        f'log.dirs={root}/broker-data', 'num.partitions=1']) + '\n')
    env = dict(os.environ, JAVA_HOME='/usr/lib/jvm/java-21-openjdk-amd64',
               KAFKA_HEAP_OPTS='-Xmx256m -Xms128m', LOG_DIR=str(root / 'logs'))
    cluster = base64.urlsafe_b64encode(uuid.uuid4().bytes).decode().rstrip('=')
    formatted = subprocess.run([str(distribution / 'bin/kafka-storage.sh'), 'format', '-t', cluster,
                               '-c', str(properties)], env=env, capture_output=True, timeout=30)
    if formatted.returncode:
        raise EnvironmentError('test_failed', 'e2e_broker_format', 'kafka')
    manifest = root / 'profiles.json'
    private_write(manifest, json.dumps({'PLAINTEXT': {'connection': {'name': 'real',
        'bootstrap_servers': [f'localhost:{port}'], 'security_protocol': 'PLAINTEXT'},
        'files': {}, 'environments': {}}}))
    broker = None
    try:
        with (root / 'broker.log').open('w') as log:
            broker = subprocess.Popen([str(distribution / 'bin/kafka-server-start.sh'), str(properties)],
                                      env=env, stdout=log, stderr=log)
        deadline = time.monotonic() + config.data['budgets']['preparation_seconds']
        while True:
            if broker.poll() is not None:
                raise EnvironmentError('test_failed', 'e2e_broker_start', 'kafka')
            probe = subprocess.run([sys.executable, str(REPO / 'tools/test_environment/broker_addresses.py'),
                                    str(manifest), 'PLAINTEXT'], capture_output=True, timeout=5)
            if not probe.returncode:
                break
            if time.monotonic() >= deadline:
                raise EnvironmentError('preparation_timeout', 'e2e_broker_metadata', 'kafka')
            time.sleep(.2)
        plan = root / 'plan.json'
        suffix = uuid.uuid4().hex[:12]
        private_write(plan, json.dumps({'manifest': str(manifest), 'workspace': str(root),
            'topic': config.data['kafka']['topic'] + '_e2e_' + suffix,
            'group': config.data['kafka']['group'] + '_e2e_' + suffix}))
        result = subprocess.run([str(REPO / 'build/output/packet_sink_e2e'), 'ps0', 'pt0', str(plan)],
                                capture_output=True, text=True, timeout=120)
        output = scrub(result.stdout + result.stderr, config)
        private_write(root / 'client.log', output)
        print(output, end='', flush=True)
        if result.returncode:
            raise EnvironmentError('test_failed', 'capture_e2e', 'kafka')
        return 0
    finally:
        if broker is not None and broker.poll() is None:
            broker.terminate()
            broker.wait(timeout=15)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except EnvironmentError as error:
        print(json.dumps(error.result()), flush=True)
        sys.exit(1)
    except (OSError, ValueError, subprocess.SubprocessError):
        print(json.dumps({'category': 'execution_denied', 'stage': 'isolated_capture'}), flush=True)
        sys.exit(1)

# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real confirmed result publication using the shared identity fixture and private plans."""
import argparse
import json
import os
import base64
import socket
import time
from pathlib import Path
import subprocess
import sys
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools'))
from test_environment.config import Config, EnvironmentError, STATE
from test_environment.fixture import prepare_kafka, private_write, runtime_command, kafka_distribution, prepare_network
from test_environment.runner import kafka_probe, scrub


def controlled_disconnect(config, location, root):
    """Stop a broker created solely for this case, never the shared fixture or user services."""
    case = root / 'disconnect'
    case.mkdir(mode=0o700)
    distribution = kafka_distribution(config)
    token = uuid.uuid4().hex[:12]
    name = 'flowsql-sink-disconnect-' + token
    host = 'localhost' if location == 'native' else name
    ports = []
    while len(ports) < 2:
        with socket.socket() as endpoint:
            endpoint.bind(('127.0.0.1', 0))
            port = endpoint.getsockname()[1]
        if port not in ports:
            ports.append(port)
    port, controller = ports
    properties = case / 'broker.properties'
    private_write(properties, '\n'.join([
        'process.roles=broker,controller', 'node.id=1', 'controller.listener.names=CONTROLLER',
        f'controller.quorum.voters=1@localhost:{controller}',
        f'listeners=PLAINTEXT://0.0.0.0:{port},CONTROLLER://0.0.0.0:{controller}',
        f'advertised.listeners=PLAINTEXT://{host}:{port}',
        'listener.security.protocol.map=PLAINTEXT:PLAINTEXT,CONTROLLER:PLAINTEXT',
        'inter.broker.listener.name=PLAINTEXT', 'auto.create.topics.enable=false',
        'offsets.topic.replication.factor=1', 'transaction.state.log.replication.factor=1',
        'transaction.state.log.min.isr=1', 'group.initial.rebalance.delay.ms=0',
        f'log.dirs={case}/broker-data', 'num.partitions=1']) + '\n')
    env = dict(os.environ, JAVA_HOME='/usr/lib/jvm/java-21-openjdk-amd64',
               KAFKA_HEAP_OPTS='-Xmx256m -Xms128m', LOG_DIR=str(case / 'logs'))
    cluster = base64.urlsafe_b64encode(uuid.uuid4().bytes).decode().rstrip('=')
    storage = subprocess.run([str(distribution / 'bin/kafka-storage.sh'), 'format', '-t', cluster,
        '-c', str(properties)], env=env, capture_output=True, text=True, timeout=30)
    if storage.returncode:
        raise EnvironmentError('test_failed', 'disconnect_broker_format', 'kafka')
    command = [str(distribution / 'bin/kafka-server-start.sh'), str(properties)]
    if location == 'docker':
        network = prepare_network(config)
        command = ['docker', 'run', '--rm', '--name', name, '--network', network,
            '--network-alias', host, '--label', 'flowsql.test.project=' + config.data['project'],
            '-v', f'{distribution}:{distribution}:ro', '-v', f'{case}:{case}',
            '-e', 'JAVA_HOME=' + env['JAVA_HOME'], '-e', 'KAFKA_HEAP_OPTS=' + env['KAFKA_HEAP_OPTS'],
            '-e', 'LOG_DIR=' + env['LOG_DIR'],
            config.data['images']['harness'], *command]
    broker = client = None
    try:
        with (case / 'broker.log').open('w') as log:
            broker = subprocess.Popen(command, env=env, stdout=log, stderr=log, start_new_session=True)
        manifest = case / 'profiles.json'
        private_write(manifest, json.dumps({'PLAINTEXT': {'connection': {'name':'real',
            'bootstrap_servers':[f'{host}:{port}'], 'security_protocol':'PLAINTEXT'},
            'files':{}, 'environments':{}}}))
        deadline = time.monotonic() + config.data['budgets']['preparation_seconds']
        while True:
            if broker.poll() is not None:
                raise EnvironmentError('test_failed', 'disconnect_broker_start', 'kafka')
            probe = subprocess.run(runtime_command(config, ['python3',
                str(REPO / 'tools/test_environment/broker_addresses.py'), str(manifest), 'PLAINTEXT'],
                location=location), capture_output=True, text=True, timeout=5)
            if probe.returncode == 0:
                break
            if time.monotonic() >= deadline:
                raise EnvironmentError('preparation_timeout', 'disconnect_broker_metadata', 'kafka')
            time.sleep(.2)
        plan = case / 'plan.json'
        private_write(plan, json.dumps({'manifest':str(manifest), 'profile':'PLAINTEXT',
            'topic':config.data['kafka']['topic'] + '_disconnect_' + token,
            'group':config.data['kafka']['group'] + '_disconnect_' + token,
            'workspace':str(case), 'disconnect':True}))
        client = subprocess.Popen(runtime_command(config, [str(REPO / 'build/output/kafka_sink_real'), str(plan)],
            location=location), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 30
        while not (case / 'ready').exists():
            if client.poll() is not None or time.monotonic() >= deadline:
                raise EnvironmentError('test_failed', 'disconnect_client_ready', 'kafka')
            time.sleep(.05)
        if location == 'docker':
            stopped = subprocess.run(['docker', 'stop', '-t', '5', name], capture_output=True, text=True, timeout=15)
            if stopped.returncode:
                raise EnvironmentError('test_failed', 'disconnect_broker_stop', 'kafka')
        else:
            broker.terminate()
        broker.wait(timeout=15)
        private_write(case / 'disconnected', '1')
        stdout, stderr = client.communicate(timeout=15)
        output = scrub(stdout + stderr, config)
        private_write(case / 'client.log', output)
        print(output, end='', flush=True)
        if client.returncode:
            raise EnvironmentError('test_failed', 'controlled_disconnect', 'kafka')
    finally:
        if client is not None and client.poll() is None:
            client.terminate()
            client.communicate(timeout=10)
        if broker is not None and broker.poll() is None:
            if location == 'docker':
                subprocess.run(['docker', 'stop', '-t', '5', name], capture_output=True, timeout=15)
            else:
                broker.terminate()
            broker.wait(timeout=15)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--location', choices=('native', 'docker'), required=True)
    parser.add_argument('--config')
    args = parser.parse_args()
    config = Config(args.config)
    manifest = prepare_kafka(config, args.location)
    profiles = json.loads(manifest.read_text())
    expected = {'PLAINTEXT', 'SSL', 'MTLS', 'PLAIN', 'SCRAM-SHA-256', 'SCRAM-SHA-512',
                'SASL_SSL-PLAIN', 'SASL_SSL-SCRAM-SHA-256', 'SASL_SSL-SCRAM-SHA-512',
                'GSSAPI', 'OAUTHBEARER'}
    if set(profiles) != expected:
        raise EnvironmentError('config_invalid', 'sink_profiles', 'kafka')
    root = STATE / config.data['project'] / ('sink-' + args.location + '-' + uuid.uuid4().hex)
    root.mkdir(parents=True, mode=0o700)
    for profile in profiles:
        # From the actual process/container: authentication and metadata only, before data fixtures.
        kafka_probe(config, args.location, profile, manifest)
        case = root / profile
        case.mkdir(mode=0o700)
        suffix = uuid.uuid4().hex[:12]
        plan = case / 'plan.json'
        private_write(plan, json.dumps({'manifest': str(manifest), 'profile': profile,
            'topic': config.data['kafka']['topic'] + '_sink_' + suffix,
            'group': config.data['kafka']['group'] + '_sink_' + suffix,
            'workspace': str(case)}))
        binary = REPO / 'build/output/kafka_sink_real'
        command = runtime_command(config, [str(binary), str(plan)], location=args.location)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=240)
        except subprocess.TimeoutExpired:
            raise EnvironmentError('test_failed', 'sink_timeout', 'kafka', profile) from None
        output = scrub(result.stdout + result.stderr, config)
        private_write(case / 'client.log', output)
        print(output, end='', flush=True)
        if result.returncode:
            raise EnvironmentError('test_failed', 'result_sink', 'kafka', profile)
        print(json.dumps({'profile': profile, 'location': args.location, 'status': 'passed'}), flush=True)
    controlled_disconnect(config, args.location, root)
    print(json.dumps({'location': args.location, 'profiles': len(profiles), 'status': 'passed'}))


if __name__ == '__main__':
    try:
        main()
    except EnvironmentError as error:
        print(json.dumps(error.result()), flush=True)
        sys.exit(1)
    except (OSError, ValueError, KeyError):
        print(json.dumps({'category': 'config_invalid', 'stage': 'sink_fixture'}), flush=True)
        sys.exit(1)

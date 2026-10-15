# Copyright (C) 2026 LIHUO. All rights reserved.
# Licensed under the MIT License.
"""Real structured consumption using the shared identity fixture and private plans."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'tools'))
from test_environment.config import Config, EnvironmentError, STATE
from test_environment.fixture import prepare_kafka, private_write, runtime_command
from test_environment.runner import kafka_probe, scrub


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
        raise EnvironmentError('config_invalid', 'source_profiles', 'kafka')
    root = STATE / config.data['project'] / ('source-' + args.location + '-' + uuid.uuid4().hex)
    root.mkdir(parents=True, mode=0o700)
    for profile in profiles:
        # From the actual process/container: authentication and metadata only, before data fixtures.
        kafka_probe(config, args.location, profile, manifest)
        case = root / profile
        case.mkdir(mode=0o700)
        suffix = uuid.uuid4().hex[:12]
        plan = case / 'plan.json'
        private_write(plan, json.dumps({'manifest': str(manifest), 'profile': profile,
            'topic': config.data['kafka']['topic'] + '_source_' + suffix,
            'group': config.data['kafka']['group'] + '_source_' + suffix,
            'workspace': str(case)}))
        binary = REPO / 'build/output/kafka_source_real'
        command = runtime_command(config, [str(binary), str(plan)], location=args.location)
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        except subprocess.TimeoutExpired:
            raise EnvironmentError('test_failed', 'source_timeout', 'kafka', profile) from None
        output = scrub(result.stdout + result.stderr, config)
        private_write(case / 'client.log', output)
        print(output, end='', flush=True)
        if result.returncode:
            raise EnvironmentError('test_failed', 'structured_source', 'kafka', profile)
        print(json.dumps({'profile': profile, 'location': args.location, 'status': 'passed'}), flush=True)
    print(json.dumps({'location': args.location, 'profiles': len(profiles), 'status': 'passed'}))


if __name__ == '__main__':
    try:
        main()
    except EnvironmentError as error:
        print(json.dumps(error.result()), flush=True)
        sys.exit(1)
    except (OSError, ValueError, KeyError):
        print(json.dumps({'category': 'config_invalid', 'stage': 'source_fixture'}), flush=True)
        sys.exit(1)

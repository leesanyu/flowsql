#!/usr/bin/env python3
"""Validate actual product artifacts; explicitly build local images without publishing."""
import argparse
import json
import mmap
from pathlib import Path
import re
import subprocess

from product_version import RECORD_BEGIN, RECORD_END, parse_version, snapshot

ROOT = Path(__file__).resolve().parents[1]


def validate_info(info):
    if not isinstance(info, dict) or set(info) != {'version', 'revision', 'dirty'}:
        raise ValueError('Build identity must contain version, revision and dirty')
    parse_version(info['version'])
    if info['revision'] is not None and not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', info['revision']):
        raise ValueError('Invalid build revision')
    if info['dirty'] is not None and type(info['dirty']) is not bool:
        raise ValueError('Invalid dirty state')
    return info


def read_info(path):
    return validate_info(json.loads(Path(path).read_text(encoding='utf-8')))


def embedded_info(path):
    records = []
    begin, end = RECORD_BEGIN.encode(), RECORD_END.encode()
    with Path(path).open('rb') as stream:
        if not stream.seek(0, 2):
            raise ValueError(f'Empty artifact: {path}')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            position = 0
            while (start := data.find(begin, position)) >= 0:
                stop = data.find(end, start + len(begin))
                if stop < 0 or stop - start > 2048:
                    raise ValueError(f'Malformed embedded identity: {path}')
                records.append(validate_info(json.loads(data[start + len(begin):stop])))
                position = stop + len(end)
    if not records or any(record != records[0] for record in records):
        raise ValueError(f'Missing or conflicting embedded identity: {path}')
    return records[0]


def check_artifacts(output, worker=None, expect_version=None, expect_revision=None):
    output = Path(output)
    info = read_info(output / 'build-info.json')
    consumers = [output / 'flowsql', *sorted(output.glob('libflowsql*.so*'))]
    if not (output / 'libflowsql_web.so').is_file():
        raise ValueError('Missing Web plugin')
    scripts = sorted((output / 'static/assets').glob('*.js'))
    if not scripts:
        raise ValueError('Missing frontend JavaScript')
    # The HTML entry must reference an actual JS asset, not just an orphan metadata file.
    html = (output / 'static/index.html').read_text(encoding='utf-8')
    entries = re.findall(r'<script[^>]+src=["\']([^"\']+)["\']', html)
    if not entries or any(not (output / 'static' / name.lstrip('/')).is_file() for name in entries):
        raise ValueError('Missing frontend entry asset')
    consumers.extend(scripts)
    for path in consumers:
        if embedded_info(path) != info:
            raise ValueError(f'Artifact identity mismatch: {path}')
    for path in (output / 'static/build-info.json', worker or output / 'python/flowsql/_build_info.json'):
        if read_info(path) != info:
            raise ValueError(f'Artifact identity mismatch: {path}')
    if expect_version is not None and info['version'] != expect_version:
        raise ValueError('Image version argument does not match actual artifacts')
    if expect_revision is not None and (info['revision'] or 'unknown') != expect_revision:
        raise ValueError('Image revision argument does not match actual artifacts')
    return info


def verify_release(root, output, version):
    source = snapshot(root)
    if source['version'] != version or source['revision'] is None or source['dirty'] is not False:
        raise ValueError('Release requires requested VERSION and a known clean source identity')
    try:
        revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', '--verify',
                                            f'refs/tags/v{version}^{{commit}}'],
                                           stderr=subprocess.DEVNULL, text=True).strip()
    except subprocess.CalledProcessError as exc:
        raise ValueError(f'Missing release tag v{version}') from exc
    if revision != source['revision']:
        raise ValueError('Release tag does not resolve to current source commit')
    if check_artifacts(output) != source:
        raise ValueError('Actual artifacts do not match release source')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    artifacts = commands.add_parser('artifacts', help='Check all embedded native, Worker and frontend identities')
    artifacts.add_argument('--output', type=Path, default=ROOT / 'build/output')
    artifacts.add_argument('--worker', type=Path)
    artifacts.add_argument('--expect-version')
    artifacts.add_argument('--expect-revision')
    verify = commands.add_parser('verify', help='Read-only release validation; never creates tags or publishes')
    verify.add_argument('--root', type=Path, default=ROOT)
    verify.add_argument('--output', type=Path, default=ROOT / 'build/output')
    verify.add_argument('--version', required=True)
    image = commands.add_parser('image', help='Build a local Docker image from verified build/output')
    image.add_argument('--tag', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'artifacts':
            info = check_artifacts(args.output, args.worker, args.expect_version, args.expect_revision)
        elif args.command == 'verify':
            info = verify_release(args.root, args.output, args.version)
        else:
            info = check_artifacts(ROOT / 'build/output')
            if info != snapshot(ROOT):
                raise ValueError('Rebuild: build/output identity does not match current source')
            subprocess.run(['docker', 'build', '--build-arg', f"PRODUCT_VERSION={info['version']}",
                            '--build-arg', f"PRODUCT_REVISION={info['revision'] or 'unknown'}",
                            '-t', args.tag, '.'], cwd=ROOT, check=True)
        print(json.dumps(info))
    except (OSError, ValueError, TypeError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'product release: {exc}\n')


if __name__ == '__main__':
    main()

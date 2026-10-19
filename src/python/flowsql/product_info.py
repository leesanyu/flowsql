"""Build identity; installed/staged packages always use their captured snapshot."""
import json
from pathlib import Path


def get_build_info():
    stored = Path(__file__).with_name('_build_info.json')
    if stored.is_file():
        return json.loads(stored.read_text(encoding='utf-8'))
    # Source-tree invocation is a development entry, not a packaged artifact.
    root = Path(__file__).resolve().parents[3]
    import runpy
    return runpy.run_path(str(root / 'tools/product_version.py'))['snapshot'](root)


def version_text():
    info = get_build_info()
    dirty = 'unknown' if info['dirty'] is None else str(info['dirty']).lower()
    return f"FlowSQL {info['version']}\nrevision={info['revision'] or 'unknown'}\ndirty={dirty}\n"

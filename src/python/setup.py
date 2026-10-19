from setuptools import setup, find_packages
from setuptools.command.build_py import build_py
from setuptools.command.sdist import sdist
import json
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
if (ROOT / 'tools/product_version.py').is_file():
    tool = runpy.run_path(str(ROOT / 'tools/product_version.py'))
    info = tool['snapshot'](ROOT)
    python_version = tool['parse_version'](info['version'])[1]
else:
    # An sdist carries the snapshot captured when that source distribution was made.
    info = json.loads((HERE / 'flowsql/_build_info.json').read_text())
    import re
    python_version = re.sub(r'-(dev|alpha|beta|rc)\.',
                            lambda m: {'dev': '.dev', 'alpha': 'a', 'beta': 'b', 'rc': 'rc'}[m[1]],
                            info['version'])


class BuildPy(build_py):
    def run(self):
        super().run()
        (Path(self.build_lib) / 'flowsql/_build_info.json').write_text(json.dumps(info) + '\n')


class Sdist(sdist):
    def make_release_tree(self, base_dir, files):
        super().make_release_tree(base_dir, files)
        (Path(base_dir) / 'flowsql/_build_info.json').write_text(json.dumps(info) + '\n')

setup(
    name="flowsql-worker",
    version=python_version,
    cmdclass={'build_py': BuildPy, 'sdist': Sdist},
    package_data={'flowsql': ['_build_info.json']},
    packages=find_packages(),
    python_requires=">=3.10",
    install_requires=[
        "fastapi>=0.104.0",
        "uvicorn>=0.24.0",
        "pyarrow==22.0.0",
        "pandas>=2.0.0",
        "polars>=1.0.0",
        "httpx>=0.25.0",
    ],
    entry_points={
        "console_scripts": [
            "flowsql-worker=flowsql.worker:main",
        ],
    },
)

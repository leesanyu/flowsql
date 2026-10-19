"""Product version contract, using isolated source trees and Git repositories."""
import importlib.util
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import tarfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("product_version", ROOT / "tools/product_version.py")
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class ProductVersionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="flowsql-version-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "VERSION").write_text("0.1.0\n")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], text=True).strip()

    def repository(self):
        self.git("init", "-q")
        self.git("config", "user.email", "version-test@example.invalid")
        self.git("config", "user.name", "Version Test")
        (self.root / ".gitignore").write_text("generated/\nevidence/\nbuild/\n")
        self.git("add", "VERSION", ".gitignore")
        self.git("commit", "-qm", "initial")

    def test_versions_and_python_mapping(self):
        for suffix, pep in [("", ""), ("-dev.0", ".dev0"), ("-alpha.1", "a1"),
                            ("-beta.2", "b2"), ("-rc.3", "rc3")]:
            with self.subTest(suffix=suffix):
                self.assertEqual(version.parse_version("0.1.0" + suffix), ("0.1.0", "0.1.0" + pep))
        for bad in ["v0.1.0", "01.1.0", "0.1", "0.1.0+abc", "0.1.0-rc.01",
                    "0.1.0-rc", "0.1.0\n0.2.0", " 0.1.0", "0.1.0 "]:
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                version.parse_version(bad)

    def test_missing_or_invalid_source_fails(self):
        (self.root / "VERSION").unlink()
        with self.assertRaises(FileNotFoundError):
            version.snapshot(self.root)
        (self.root / "VERSION").write_text("v0.1.0\n")
        with self.assertRaises(ValueError):
            version.snapshot(self.root)

    def test_no_git_reports_unknown_not_parent_repository(self):
        self.assertEqual(version.snapshot(self.root), {"version": "0.1.0", "revision": None, "dirty": None})
        self.repository()
        child = self.root / "export"
        child.mkdir()
        (child / "VERSION").write_text("0.1.0\n")
        self.assertIsNone(version.snapshot(child)["revision"])

    def test_git_identity_and_each_dirty_state(self):
        self.repository()
        info = version.snapshot(self.root)
        self.assertEqual(info["revision"], self.git("rev-parse", "HEAD"))
        self.assertIs(info["dirty"], False)
        (self.root / "evidence").mkdir()
        (self.root / "evidence/test").write_text("ignored")
        self.assertIs(version.snapshot(self.root)["dirty"], False)
        (self.root / "VERSION").write_text("0.1.1\n")
        self.assertIs(version.snapshot(self.root)["dirty"], True)
        self.git("add", "VERSION")
        self.assertIs(version.snapshot(self.root)["dirty"], True)
        self.git("commit", "-qm", "version")
        self.assertIs(version.snapshot(self.root)["dirty"], False)
        self.assertNotEqual(version.snapshot(self.root)["revision"], info["revision"])
        (self.root / "new-file").write_text("untracked")
        self.assertIs(version.snapshot(self.root)["dirty"], True)

    def test_generation_is_stable_and_updates_version_and_identity(self):
        self.repository()
        output = self.root / "generated"
        version.generate(self.root, output)
        paths = list(output.iterdir())
        before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}
        version.generate(self.root, output)
        self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths})
        (self.root / "VERSION").write_text("0.2.0-rc.1\n")
        version.generate(self.root, output)
        info = json.loads((output / "build-info.json").read_text())
        self.assertEqual(info["version"], "0.2.0-rc.1")
        self.assertIs(info["dirty"], True)
        self.assertIn('"0.2.0-rc.1"', (output / "flowsql_build_info.h").read_text())

    def test_cmake_incremental_binary_updates_without_rewriting_unchanged_files(self):
        (self.root / 'tools').mkdir()
        (self.root / 'src/cmake').mkdir(parents=True)
        shutil.copyfile(ROOT / 'tools/product_version.py', self.root / 'tools/product_version.py')
        shutil.copyfile(ROOT / 'src/cmake/product_version.cmake', self.root / 'src/cmake/product_version.cmake')
        (self.root / 'src/main.cpp').write_text(
            '#include <iostream>\n#include "common/product_version.h"\n'
            'int main(){const auto& i=flowsql::GetProductBuildInfo();'
            'std::cout<<i.version<<" "<<i.revision.value_or("unknown")<<" "<<*i.dirty;}\n')
        (self.root / 'src/CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.28)
include(cmake/product_version.cmake)
project(fixture VERSION ${{FLOWSQL_NUMERIC_VERSION}} LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(identity main.cpp)
target_include_directories(identity PRIVATE "{ROOT / 'src'}")
add_dependencies(identity flowsql_version)
''')
        self.repository()
        self.git('add', 'tools', 'src')
        self.git('commit', '-qm', 'fixture')
        build = self.root / 'build'

        def command(*args):
            return subprocess.check_output(args, cwd=self.root, text=True, stderr=subprocess.STDOUT)

        command('cmake', '-B', str(build), 'src')
        command('cmake', '--build', str(build))
        initial = command(str(build / 'identity'))
        self.assertIn('0.1.0 ' + self.git('rev-parse', 'HEAD') + ' 0', initial)
        watched = [build / 'identity', build / 'generated/version/build-info.json',
                   build / 'generated/version/flowsql_build_info.h']
        times = [p.stat().st_mtime_ns for p in watched]
        command('cmake', '--build', str(build))
        self.assertEqual(times, [p.stat().st_mtime_ns for p in watched])
        (self.root / 'VERSION').write_text('0.2.0-rc.1\n')
        command('cmake', '--build', str(build))
        self.assertTrue(command(str(build / 'identity')).startswith('0.2.0-rc.1 '))
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'candidate')
        command('cmake', '--build', str(build))
        self.assertIn(self.git('rev-parse', 'HEAD') + ' 0', command(str(build / 'identity')))
        (self.root / 'untracked').write_text('dirty')
        command('cmake', '--build', str(build))
        self.assertTrue(command(str(build / 'identity')).endswith(' 1'))

    def test_worker_source_distribution_keeps_build_snapshot(self):
        package = self.root / 'src/python'
        (package / 'flowsql').mkdir(parents=True)
        (package / 'flowsql/__init__.py').write_text('')
        shutil.copyfile(ROOT / 'src/python/setup.py', package / 'setup.py')
        (self.root / 'tools').mkdir()
        shutil.copyfile(ROOT / 'tools/product_version.py', self.root / 'tools/product_version.py')
        (self.root / 'VERSION').write_text('0.1.0-rc.1\n')
        subprocess.run([sys.executable, 'setup.py', 'sdist', '--dist-dir', str(self.root / 'dist')],
                       cwd=package, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        archive = next((self.root / 'dist').glob('*.tar.gz'))
        unpacked = self.root / 'unpacked'
        with tarfile.open(archive) as stream:
            stream.extractall(unpacked, filter='data')
        exported = next(unpacked.iterdir())
        info = json.loads((exported / 'flowsql/_build_info.json').read_text())
        self.assertEqual(info, {'version': '0.1.0-rc.1', 'revision': None, 'dirty': None})
        result = subprocess.check_output([sys.executable, 'setup.py', '--version'], cwd=exported, text=True)
        self.assertEqual(result.strip(), '0.1.0rc1')
        subprocess.run([sys.executable, 'setup.py', 'build_py'], cwd=exported, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        built = next((exported / 'build').glob('lib*/flowsql/_build_info.json'))
        self.assertEqual(json.loads(built.read_text()), info)


if __name__ == "__main__":
    unittest.main()

"""Release validation rejects mixed artifacts and mismatched source/tag identities."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
import product_version
import release


class ProductReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='flowsql-release-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'VERSION').write_text('0.1.0\n')
        self.git('init', '-q')
        self.git('config', 'user.email', 'release-test@example.invalid')
        self.git('config', 'user.name', 'Release Test')
        (self.root / '.gitignore').write_text('output/\n')
        self.git('add', 'VERSION', '.gitignore')
        self.git('commit', '-qm', 'version')
        self.info = product_version.snapshot(self.root)
        self.output = self.root / 'output'
        self.worker = self.output / 'python/flowsql/_build_info.json'
        self.write_artifacts(self.info)

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], text=True).strip()

    def write(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def write_artifacts(self, info):
        raw = json.dumps(info)
        record = product_version.RECORD_BEGIN + raw + product_version.RECORD_END
        for name in ('flowsql', 'libflowsql_web.so', 'libflowsql_router.so', 'static/assets/app.js'):
            self.write(self.output / name, record)
        for name in ('build-info.json', 'static/build-info.json', 'python/flowsql/_build_info.json'):
            self.write(self.output / name, raw)
        self.write(self.output / 'static/index.html', '<script src="/assets/app.js"></script>')

    def test_matching_artifacts_and_exact_tag_pass(self):
        self.git('tag', 'v0.1.0')
        self.assertEqual(release.check_artifacts(self.output), self.info)
        self.assertEqual(release.verify_release(self.root, self.output, '0.1.0'), self.info)

    def test_each_mixed_consumer_is_rejected(self):
        changed = dict(self.info, version='0.1.1')
        for name in ('flowsql', 'libflowsql_web.so', 'libflowsql_router.so',
                     'static/assets/app.js', 'build-info.json', 'static/build-info.json',
                     'python/flowsql/_build_info.json'):
            with self.subTest(name=name):
                self.write_artifacts(self.info)
                target = self.output / name
                target.write_text(target.read_text().replace('0.1.0', '0.1.1'))
                with self.assertRaises(ValueError):
                    release.check_artifacts(self.output)

    def test_missing_identity_and_dirty_release_are_rejected(self):
        (self.output / 'libflowsql_web.so').write_text('old plugin without identity')
        with self.assertRaises(ValueError):
            release.check_artifacts(self.output)
        self.git('tag', 'v0.1.0')
        (self.root / 'untracked').write_text('dirty')
        self.write_artifacts(product_version.snapshot(self.root))
        with self.assertRaises(ValueError):
            release.verify_release(self.root, self.output, '0.1.0')

    def test_unknown_artifact_identity_cannot_pass_a_known_clean_release(self):
        self.git('tag', 'v0.1.0')
        self.write_artifacts(dict(self.info, revision=None, dirty=None))
        self.assertEqual(release.check_artifacts(self.output)['dirty'], None)
        with self.assertRaises(ValueError):
            release.verify_release(self.root, self.output, '0.1.0')

    def test_frontend_missing_entry_and_conflicting_embedded_records_are_rejected(self):
        (self.output / 'static/index.html').write_text('<script src="/assets/old.js"></script>')
        with self.assertRaises(ValueError):
            release.check_artifacts(self.output)
        self.write_artifacts(self.info)
        path = self.output / 'libflowsql_web.so'
        path.write_text(path.read_text() + product_version.RECORD_BEGIN +
                        json.dumps(dict(self.info, revision='0' * 40)) + product_version.RECORD_END)
        with self.assertRaises(ValueError):
            release.check_artifacts(self.output)
    def test_missing_tag_wrong_request_and_tag_target_are_rejected(self):
        with self.assertRaises(ValueError):
            release.verify_release(self.root, self.output, '0.1.0')
        self.git('tag', 'v0.1.0')
        with self.assertRaises(ValueError):
            release.verify_release(self.root, self.output, '0.1.1')
        self.git('commit', '--allow-empty', '-qm', 'next')
        self.write_artifacts(product_version.snapshot(self.root))
        with self.assertRaises(ValueError):
            release.verify_release(self.root, self.output, '0.1.0')

    def test_image_arguments_must_match_actual_artifacts(self):
        with self.assertRaises(ValueError):
            release.check_artifacts(self.output, expect_version='0.1.1')
        with self.assertRaises(ValueError):
            release.check_artifacts(self.output, expect_revision='unknown')

    def test_command_is_read_only_and_errors_exit_nonzero(self):
        before = self.git('status', '--porcelain')
        result = subprocess.run([sys.executable, str(ROOT / 'tools/release.py'), 'verify',
                                 '--root', str(self.root), '--output', str(self.output), '--version', '0.1.0'],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.git('tag'), '')
        self.assertEqual(self.git('status', '--porcelain'), before)


if __name__ == '__main__':
    unittest.main()

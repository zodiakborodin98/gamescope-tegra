# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package', ROOT / 'scripts/package_source.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / 'source'
        self.root.mkdir()
        # An isolated model exercising the actual packager, not the development tree.
        for path in ('LICENSE', 'COPYING', 'NOTICE.md', 'README.md', 'docs/BUILD.md',
                     'docs/LICENSING.md', 'docs/PROVENANCE.md', 'external-artifacts.lock.json',
                     'LICENSES/MIT.txt', 'LICENSES/GPL-2.0.txt', 'gamescope/LICENSE',
                     'LICENSES/Gamescope.upstream.txt'):
            dst = self.root / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_text('license or source\n')
        (self.root / 'sources.lock.json').write_text('{"source_inputs": {}, "repositories": [{"path": "subprojects/dep"}]}')

    def manifest(self):
        _, content = package.verify(self.root, require_manifest=False)
        (self.root / package.MANIFEST).write_text(content)

    def test_roundtrip_excludes_build_downloads_git_and_dependencies(self):
        for path in ('_work/compiled', 'downloads/archive', '.git/config',
                     'gamescope/subprojects/dep/private'):
            dst = self.root / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(b'\x7fELF ignored local payload')
        self.manifest()
        dest = self.base / 'source.zip'
        package.archive(dest, self.root)
        with zipfile.ZipFile(dest) as archive:
            self.assertTrue(all('compiled' not in p and 'private' not in p and '/.git/' not in p for p in archive.namelist()))
            self.assertIn('gamescope-tegra/SOURCE-MANIFEST.sha256', archive.namelist())

    def test_rejects_compiled_binary(self):
        (self.root / 'payload').write_bytes(b'\x7fELF data')
        with self.assertRaisesRegex(ValueError, 'Compiled executable'):
            package.verify(self.root, require_manifest=False)

    def test_rejects_runtime_library(self):
        (self.root / 'driver.so.1').write_bytes(b'payload')
        with self.assertRaisesRegex(ValueError, 'payload'):
            package.verify(self.root, require_manifest=False)

    def test_rejects_credentials(self):
        (self.root / 'key').write_text('-----BEGIN ' + 'OPENSSH PRIVATE KEY-----')
        with self.assertRaisesRegex(ValueError, 'Credential'):
            package.verify(self.root, require_manifest=False)

    def test_rejects_frozen_input_mismatch(self):
        (self.root / 'sources.lock.json').write_text('{"source_inputs":{"LICENSE":{"sha256":"bad"}},"repositories":[]}')
        with self.assertRaisesRegex(ValueError, 'Frozen source'):
            package.verify(self.root, require_manifest=False)

    def test_rejects_manifest_drift(self):
        self.manifest()
        (self.root / 'README.md').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'manifest mismatch'):
            package.verify(self.root)

    def test_archive_no_overwrite(self):
        self.manifest()
        dest = self.base / 'source.zip'
        dest.write_bytes(b'keep')
        with self.assertRaises(ValueError):
            package.archive(dest, self.root)
        self.assertEqual(dest.read_bytes(), b'keep')

    def test_archive_outside_source(self):
        self.manifest()
        with self.assertRaises(ValueError):
            package.archive(self.root / 'source.zip', self.root)


if __name__ == '__main__':
    unittest.main()

# SPDX-License-Identifier: GPL-2.0-or-later
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/fetch_external.py'
spec = importlib.util.spec_from_file_location('external', SCRIPT)
external = importlib.util.module_from_spec(spec)
spec.loader.exec_module(external)


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dest = Path(self.tmp.name) / 'artifact'
        self.data = b'verified artifact bytes'
        self.digest = hashlib.sha256(self.data).hexdigest()

    def copy(self, **kw):
        return external.copy_verified(io.BytesIO(self.data), self.dest, self.digest, **kw)

    def assert_empty(self):
        self.assertEqual(list(self.dest.parent.iterdir()), [])

    def test_success(self):
        self.assertEqual(self.copy(expected_size=len(self.data)), len(self.data))
        self.assertEqual(self.dest.read_bytes(), self.data)
        self.assertEqual(len(list(self.dest.parent.iterdir())), 1)

    def test_existing_output(self):
        self.dest.write_bytes(b'previous')
        with self.assertRaises(ValueError):
            self.copy()
        self.assertEqual(self.dest.read_bytes(), b'previous')

    def test_hash_mismatch(self):
        with self.assertRaises(ValueError):
            external.copy_verified(io.BytesIO(self.data), self.dest, '0' * 64)
        self.assert_empty()

    def test_bad_pin(self):
        with self.assertRaises(ValueError):
            external.copy_verified(io.BytesIO(self.data), self.dest, None)
        self.assert_empty()

    def test_size_mismatch(self):
        with self.assertRaises(ValueError):
            self.copy(expected_size=1)
        self.assert_empty()

    def test_byte_bound(self):
        with self.assertRaises(ValueError):
            self.copy(max_bytes=1)
        self.assert_empty()

    def test_deadline(self):
        with self.assertRaises(TimeoutError):
            self.copy(deadline=time.monotonic() - 1)
        self.assert_empty()

    def test_read_error(self):
        class Broken:
            def read(self, size):
                raise OSError('read failed')
        with self.assertRaises(OSError):
            external.copy_verified(Broken(), self.dest, self.digest)
        self.assert_empty()

    def test_atomic_install_failure(self):
        with patch.object(external.os, 'link', side_effect=OSError('no disk / unsupported filesystem')):
            with self.assertRaises(OSError):
                self.copy()
        self.assert_empty()

    def test_destination_race(self):
        real_link = external.os.link
        def collide(src, dest):
            Path(dest).write_bytes(b'other writer')
            real_link(src, dest)
        with patch.object(external.os, 'link', side_effect=collide):
            with self.assertRaises(FileExistsError):
                self.copy()
        self.assertEqual(self.dest.read_bytes(), b'other writer')
        self.assertEqual(len(list(self.dest.parent.iterdir())), 1)

    def test_official_https(self):
        external.validate_url('https://repo.steampowered.com/steam/', {'repo.steampowered.com'})

    def test_unsafe_urls(self):
        for url in ('http://repo.steampowered.com/a', 'https://evil.test/a',
                    'https://repo.steampowered.com.evil.test/a',
                    'https://user:secret@repo.steampowered.com/a',
                    'https://repo.steampowered.com:444/a'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                external.validate_url(url, {'repo.steampowered.com'})

    def test_redirect_outside_allowlist(self):
        handler = external.OfficialRedirect({'repo.steampowered.com'})
        with self.assertRaises(ValueError):
            handler.redirect_request(None, None, 302, '', {}, 'https://evil.test/a')


if __name__ == '__main__':
    unittest.main()

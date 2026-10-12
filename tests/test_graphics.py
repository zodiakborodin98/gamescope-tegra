# SPDX-License-Identifier: GPL-2.0-or-later
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from fetch_graphics import tree_hash
from build_graphics import validate_llvm_metadata


class GraphicsSourceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.source = Path(self.tmp.name)

    def test_tree_detects_content_and_empty_directory_changes(self):
        file = self.source / 'source.c'
        file.write_text('source')
        before = tree_hash(self.source)
        file.write_text('changed')
        self.assertNotEqual(before, tree_hash(self.source))
        before = tree_hash(self.source)
        (self.source / 'empty').mkdir()
        self.assertNotEqual(before, tree_hash(self.source))

    def test_receipt_excluded(self):
        before = tree_hash(self.source)
        (self.source / '.source-receipt.json').write_text('receipt')
        self.assertEqual(before, tree_hash(self.source))

    def test_directory_symlink_refused(self):
        (self.source / 'real').mkdir()
        try:
            (self.source / 'link').symlink_to('real', target_is_directory=True)
        except OSError:
            self.skipTest('symlink creation requires host privileges')
        with self.assertRaisesRegex(ValueError, 'directory symlink'):
            tree_hash(self.source)

    def test_file_symlink_not_equivalent_to_regular_file(self):
        file = self.source / 'source.c'
        file.write_text('target.c')
        before = tree_hash(self.source)
        file.unlink()
        try:
            file.symlink_to('target.c')
        except OSError:
            self.skipTest('symlink creation requires host privileges')
        self.assertNotEqual(before, tree_hash(self.source))

    def test_all_native_initializers_required(self):
        patch = (Path(__file__).resolve().parents[1] / 'graphics-stack/patches/llvm-aarch64-native-metadata.patch').read_text()
        metadata = '\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+#define'))
        validate_llvm_metadata(metadata)
        with self.assertRaises(SystemExit):
            validate_llvm_metadata(metadata.replace('LLVMInitializeAArch64AsmParser', 'LLVMInitializeX86AsmParser'))
        with self.assertRaises(SystemExit):
            validate_llvm_metadata(metadata + '\n#define LLVM_NATIVE_ARCH X86')
        with self.assertRaises(SystemExit):
            validate_llvm_metadata(metadata + '\n#define LLVM_NATIVE_ARCH X86 /* final override */')
        with self.assertRaises(SystemExit):
            validate_llvm_metadata(metadata + '\n#define LLVM_NATIVE_ARCH')


if __name__ == '__main__':
    unittest.main()

import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from setup_dependencies import (
    IMPORT_LIBRARIES, cached_archive, ensure_dependency,
    generate_import_libraries, validate_member_path,
)


class CachedArchiveTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.cache = Path(self.directory.name)
        self.data = b"pinned dependency archive"
        self.dependency = {
            "name": "example", "archive": "example.tar.gz",
            "url": "https://example.com/example.tar.gz",
            "sha256": hashlib.sha256(self.data).hexdigest(),
        }

    @patch("setup_dependencies.urlopen")
    def test_valid_cache_is_reused(self, request):
        archive = self.cache / self.dependency["archive"]
        archive.write_bytes(self.data)
        self.assertEqual(cached_archive(self.dependency, self.cache), archive)
        request.assert_not_called()

    @patch("setup_dependencies.urlopen")
    def test_invalid_existing_cache_is_preserved(self, request):
        archive = self.cache / self.dependency["archive"]
        archive.write_bytes(b"keep this file")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            cached_archive(self.dependency, self.cache)
        self.assertEqual(archive.read_bytes(), b"keep this file")
        request.assert_not_called()

    @patch("setup_dependencies.urlopen")
    def test_offline_missing_archive_does_not_download(self, request):
        with self.assertRaisesRegex(RuntimeError, "offline"):
            cached_archive(self.dependency, self.cache, offline=True)
        self.assertEqual(list(self.cache.iterdir()), [])
        request.assert_not_called()

    @patch("setup_dependencies.urlopen", side_effect=OSError("download failed"))
    def test_failed_download_removes_only_its_temporary_file(self, request):
        with self.assertRaisesRegex(OSError, "download failed"):
            cached_archive(self.dependency, self.cache)
        self.assertEqual(list(self.cache.iterdir()), [])
        request.assert_called_once()

    @patch("setup_dependencies.urlopen")
    def test_verified_download_is_published(self, request):
        request.return_value = io.BytesIO(self.data)
        archive = cached_archive(self.dependency, self.cache)
        self.assertEqual(archive.read_bytes(), self.data)
        self.assertEqual(list(self.cache.iterdir()), [archive])

    @patch("setup_dependencies.urlopen")
    def test_invalid_download_is_not_published(self, request):
        request.return_value = io.BytesIO(b"wrong archive")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            cached_archive(self.dependency, self.cache)
        self.assertEqual(list(self.cache.iterdir()), [])


class ArchivePathTests(unittest.TestCase):
    def test_expected_relative_paths_are_accepted(self):
        for name in ("package/", "./package/include/file.h", "package\\include\\file.h"):
            validate_member_path(name, "package")

    def test_unsafe_or_unexpected_paths_are_rejected(self):
        for name in ("/package/file", "package/../file", "C:/package/file",
                     "package/file:stream", "\\\\server\\package\\file", "other/file"):
            with self.subTest(path=name), self.assertRaises(ValueError):
                validate_member_path(name, "package")


class ExistingDependencyTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.dependency = {
            "name": "example", "destination": "third_party/example",
            "required_files": ["include/example.h"],
        }
        self.header = self.root / "third_party/example/include/example.h"
        self.header.parent.mkdir(parents=True)

    @patch("setup_dependencies.cached_archive")
    def test_complete_existing_tree_is_left_unchanged(self, download):
        self.header.write_text("local modifications", encoding="utf-8")
        ensure_dependency(self.root, self.dependency, self.root / "cache")
        self.assertEqual(self.header.read_text(encoding="utf-8"), "local modifications")
        download.assert_not_called()

    @patch("setup_dependencies.cached_archive")
    def test_incomplete_existing_tree_is_not_replaced(self, download):
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            ensure_dependency(self.root, self.dependency, self.root / "cache")
        self.assertTrue(self.header.parent.is_dir())
        download.assert_not_called()


class ImportLibraryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        tool = self.root / "tools/mingw64/bin/dlltool.exe"
        tool.parent.mkdir(parents=True)
        tool.write_bytes(b"test tool")
        self.library_directory = self.root / "sdk/lib"
        self.library_directory.mkdir(parents=True)
        definitions = self.root / "sdk/def"
        definitions.mkdir()
        for definition, library in IMPORT_LIBRARIES:
            (definitions / definition).write_text('LIBRARY "test.dll"\nEXPORTS\nexample\n', encoding="utf-8")
            (self.library_directory / library).write_bytes(b"original library")

    @patch("setup_dependencies.subprocess.run")
    def test_existing_imports_are_left_unchanged_by_default(self, run):
        generate_import_libraries(self.root)
        run.assert_not_called()
        for _, library in IMPORT_LIBRARIES:
            self.assertEqual((self.library_directory / library).read_bytes(), b"original library")

    @patch("setup_dependencies.subprocess.run")
    def test_failed_regeneration_preserves_original_imports(self, run):
        def generate(command, **kwargs):
            if run.call_count == 2:
                raise subprocess.CalledProcessError(1, command)
            (Path(kwargs["cwd"]) / command[-1]).write_bytes(b"new library")

        run.side_effect = generate
        with self.assertRaises(subprocess.CalledProcessError):
            generate_import_libraries(self.root, regenerate=True)
        for _, library in IMPORT_LIBRARIES:
            self.assertEqual((self.library_directory / library).read_bytes(), b"original library")
        self.assertEqual(len(list(self.library_directory.iterdir())), len(IMPORT_LIBRARIES))

    @patch("setup_dependencies.subprocess.run")
    def test_regeneration_uses_x64_and_publishes_all_libraries(self, run):
        run.side_effect = lambda command, **kwargs: (Path(kwargs["cwd"]) / command[-1]).write_bytes(b"new library")
        generate_import_libraries(self.root, regenerate=True)
        self.assertEqual(run.call_count, len(IMPORT_LIBRARIES))
        for call in run.call_args_list:
            self.assertEqual(call.args[0][1:3], ["-m", "i386:x86-64"])
            self.assertEqual(Path(call.args[0][-1]).name, call.args[0][-1])
        for _, library in IMPORT_LIBRARIES:
            self.assertEqual((self.library_directory / library).read_bytes(), b"new library")


class PinnedManifestTests(unittest.TestCase):
    def test_manifest_has_exact_versions_and_sha256_pins(self):
        manifest = json.loads((Path(__file__).parent / "dependencies.json").read_text(encoding="utf-8"))
        dependencies = manifest["dependencies"]
        self.assertEqual({item["name"]: item["version"] for item in dependencies}, {
            "mingw64": "4.9.4-posix-seh-rt_v5-rev0", "lmms": "1.2.2",
            "qtbase": "5.4.2", "libsamplerate": "0.1.8",
        })
        for dependency in dependencies:
            with self.subTest(name=dependency["name"]):
                self.assertRegex(dependency["sha256"], r"^[0-9a-f]{64}$")
                self.assertTrue(dependency["url"].startswith("https://"))


if __name__ == "__main__":
    unittest.main()

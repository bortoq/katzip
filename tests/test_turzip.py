import os
import pathlib
import resource
import signal
import subprocess
import tempfile
import unittest
import zipfile


PROGRAM = pathlib.Path(__file__).resolve().parents[1] / "turzip"
CONFIG = pathlib.Path(__file__).resolve().parents[1] / "turzip.ini"


class TurzipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def run_turzip(self, *arguments, **kwargs):
        return subprocess.run(
            [str(PROGRAM), *arguments], cwd=self.root, capture_output=True, **kwargs
        )

    def test_utf8_empty_file_and_exact_zip_overhead(self):
        name = "текст.fb2"
        (self.root / name).write_bytes(b"")
        result = self.run_turzip("-1", "archive", name)
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = (self.root / "archive.zip").read_bytes()
        with zipfile.ZipFile(self.root / "archive.zip") as opened:
            info = opened.infolist()[0]
            self.assertEqual(info.filename, name)
            self.assertEqual(opened.read(name), b"")
            self.assertTrue(info.flag_bits & 0x0800)
            self.assertEqual(len(archive) - info.compress_size, 98 + 2 * len(name.encode()))

    def test_existing_archive_survives_write_error(self):
        old = self.root / "archive.zip"
        old.write_bytes(b"previous archive")
        (self.root / "large.bin").write_bytes(os.urandom(8192))

        def limit_output():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        result = self.run_turzip(
            "-1", "archive.zip", "large.bin", preexec_fn=limit_output
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(old.read_bytes(), b"previous archive")
        self.assertEqual(list(self.root.glob("archive.zip.tmp.*")), [])

    def test_replacing_archive_keeps_its_permissions(self):
        archive = self.root / "archive.zip"
        archive.write_bytes(b"previous archive")
        archive.chmod(0o640)
        (self.root / "input.txt").write_text("new content")
        result = self.run_turzip("-1", "archive.zip", "input.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(archive.stat().st_mode & 0o777, 0o640)
        with zipfile.ZipFile(archive) as opened:
            self.assertEqual(opened.read("input.txt"), b"new content")

    def test_zip_overhead_for_multiple_files(self):
        (self.root / "one.txt").write_bytes(b"one")
        (self.root / "two.txt").write_bytes(b"two")
        result = self.run_turzip("-1", "archive", "one.txt", "two.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = self.root / "archive.zip"
        with zipfile.ZipFile(archive) as opened:
            entries = opened.infolist()
            expected_overhead = 22 + sum(
                76 + 2 * len(entry.filename.encode()) for entry in entries
            )
            self.assertEqual(
                archive.stat().st_size - sum(entry.compress_size for entry in entries),
                expected_overhead,
            )

    def test_unsafe_config_is_rejected(self):
        (self.root / "input.txt").write_text("test")
        config = self.root / "custom.ini"
        environment = dict(os.environ, TURZIP_INI=str(config))
        for key, replacement in (
            ("i_maximum_block_size = 1000000", "i_maximum_block_size = 1000001"),
            ("i_min_start_fp = -6", "i_min_start_fp = -2147483648"),
        ):
            config.write_text(CONFIG.read_text().replace(key, replacement))
            result = self.run_turzip("-9", "archive", "input.txt", env=environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid settings", result.stderr)
            self.assertFalse((self.root / "archive.zip").exists())

    def test_recursive_masks_select_files_in_subdirectories(self):
        (self.root / "src" / "nested").mkdir(parents=True)
        for name in ("src/a.txt", "src/nested/b.txt", "src/nested/c.md", "src/other.bin"):
            (self.root / name).write_text(name)
        result = subprocess.run(
            ["bash", "-c", 'exec "$1" -r -1 selection src @*.txt @*.md', "bash", str(PROGRAM)],
            cwd=self.root, capture_output=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "selection.zip") as opened:
            self.assertEqual(set(opened.namelist()), {
                "src/a.txt", "src/nested/b.txt", "src/nested/c.md"
            })

        result = subprocess.run(
            ["bash", "-c", 'exec "$1" -r -1 current @*.txt', "bash", str(PROGRAM)],
            cwd=self.root, capture_output=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "current.zip") as opened:
            self.assertEqual(set(opened.namelist()), {"src/a.txt", "src/nested/b.txt"})

    def test_explicit_file_does_not_expand_to_its_extension(self):
        (self.root / "nested").mkdir()
        (self.root / "report.txt").write_text("report")
        (self.root / "nested" / "private.txt").write_text("private")
        result = self.run_turzip("-r", "-1", "selection", "report.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "selection.zip") as opened:
            self.assertEqual(opened.namelist(), ["report.txt"])

    def test_path_mask_uses_root_relative_posix_glob(self):
        (self.root / "src" / "nested").mkdir(parents=True)
        (self.root / "src" / "nested" / "file1.txt").write_text("one")
        (self.root / "src" / "nested" / "fileA.txt").write_text("other")
        (self.root / "src" / "file2.txt").write_text("top")
        result = self.run_turzip("-r", "-1", "selection", "src", "@nested/file[0-9].txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "selection.zip") as opened:
            self.assertEqual(opened.namelist(), ["src/nested/file1.txt"])


if __name__ == "__main__":
    unittest.main()

import os
import pathlib
import resource
import signal
import struct
import subprocess
import tempfile
import unittest
import zipfile


PROGRAM = pathlib.Path(__file__).resolve().parents[1] / "katzip"
CONFIG = pathlib.Path(__file__).resolve().parents[1] / "katzip.ini"


class KatzipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def run_katzip(self, *arguments, **kwargs):
        return subprocess.run(
            [str(PROGRAM), *arguments], cwd=self.root, capture_output=True, **kwargs
        )

    def test_utf8_empty_file_and_exact_zip_overhead(self):
        name = "текст.fb2"
        (self.root / name).write_bytes(b"")
        result = self.run_katzip("-1", "archive", name)
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = (self.root / "archive.zip").read_bytes()
        with zipfile.ZipFile(self.root / "archive.zip") as opened:
            info = opened.infolist()[0]
            self.assertEqual(info.filename, name)
            self.assertEqual(opened.read(name), b"")
            self.assertTrue(info.flag_bits & 0x0800)
            self.assertEqual(len(archive) - info.compress_size, 98 + 2 * len(name.encode()))

    def test_help_text_and_optional_inputs(self):
        expected = (
            "KATZip v1.1 - Deflating with extreme devotion.\n"
            "Dedicated to the memory of Phil Katz (1962-2000), the father of ZIP.\n"
            "\n"
            "Usage:   katzip [-1..-9] [-r] <archive.zip> [[@]input_files...]\n"
            "Example: katzip APPNOTE APPNOTE.TXT\n"
        )
        result = self.run_katzip("--help")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.decode(), expected)
        self.assertEqual(result.stderr, b"")
        result = self.run_katzip()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stderr.decode(), expected)

    def test_no_inputs_uses_star_mask_at_selected_depth(self):
        (self.root / "top.txt").write_text("top")
        (self.root / ".hidden").write_text("hidden")
        (self.root / "nested").mkdir()
        (self.root / "nested" / "deep.txt").write_text("deep")
        (self.root / "flat.zip").write_bytes(b"old archive")
        result = self.run_katzip("-1", "flat")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "flat.zip") as opened:
            self.assertEqual(opened.namelist(), ["top.txt"])
        (self.root / "flat.zip").unlink()
        result = self.run_katzip("-r", "-1", "deep")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "deep.zip") as opened:
            self.assertEqual(set(opened.namelist()), {"top.txt", "nested/deep.txt"})

    def test_final_percentage_is_compressed_size_ratio(self):
        data = b"compressible text " * 1000
        (self.root / "text.txt").write_bytes(data)
        result = self.run_katzip("-1", "archive", "text.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as opened:
            info = opened.getinfo("text.txt")
            ratio = 100 * info.compress_size / info.file_size
        lines = result.stderr.decode().replace("\r", "\n").splitlines()
        self.assertEqual(lines[-1], f"text.txt {ratio:.2f}%")
        self.assertNotIn("100.00%", result.stderr.decode())

    def test_existing_archive_survives_write_error(self):
        old = self.root / "archive.zip"
        old.write_bytes(b"previous archive")
        (self.root / "large.bin").write_bytes(os.urandom(8192))

        def limit_output():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        result = self.run_katzip(
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
        result = self.run_katzip("-1", "archive.zip", "input.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(archive.stat().st_mode & 0o777, 0o640)
        with zipfile.ZipFile(archive) as opened:
            self.assertEqual(opened.read("input.txt"), b"new content")

    def test_zip_overhead_for_multiple_files(self):
        (self.root / "one.txt").write_bytes(b"one")
        (self.root / "two.txt").write_bytes(b"two")
        result = self.run_katzip("-1", "archive", "one.txt", "two.txt")
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

    def test_level_nine_writes_valid_raw_deflate(self):
        content = b"foo bar baz\n" * 300 + bytes(range(256))
        (self.root / "pattern.bin").write_bytes(content)
        result = self.run_katzip("-9", "archive", "pattern.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as opened:
            self.assertEqual(opened.read("pattern.bin"), content)
            self.assertIsNone(opened.testzip())
            ratio = 100 * opened.getinfo("pattern.bin").compress_size / len(content)
        lines = result.stderr.decode().replace("\r", "\n").splitlines()
        self.assertEqual(lines[-1].rstrip(), f"pattern.bin {ratio:.2f}%")

    def test_zip_level_hint_bits(self):
        data = b"ZIP level hints and compression. " * 40
        (self.root / "text.txt").write_bytes(data)
        for level in range(1, 10):
            result = self.run_katzip(f"-{level}", "archive", "text.txt")
            self.assertEqual(result.returncode, 0, result.stderr)
            archive = (self.root / "archive.zip").read_bytes()
            expected = 0x0006 if level <= 3 else 0x0004 if level <= 6 else 0x0000 if level <= 8 else 0x0002
            with zipfile.ZipFile(self.root / "archive.zip") as opened:
                info = opened.getinfo("text.txt")
                self.assertEqual(info.compress_type, zipfile.ZIP_DEFLATED)
                self.assertEqual(opened.read("text.txt"), data)
                self.assertEqual(info.flag_bits & 0x0006, expected)
                self.assertEqual(struct.unpack_from("<H", archive, 6)[0] & 0x0006, expected)
        (self.root / "random.bin").write_bytes(os.urandom(65536))
        result = self.run_katzip("-1", "stored", "random.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "stored.zip") as opened:
            info = opened.getinfo("random.bin")
            self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
            self.assertEqual(info.flag_bits & 0x0006, 0)

    def test_fast_levels_and_store_fallback(self):
        content = b"A short repeated sentence.\n" * 1000
        (self.root / "text.txt").write_bytes(content)
        (self.root / "random.bin").write_bytes(os.urandom(65536))
        for level in range(1, 7):
            result = self.run_katzip(f"-{level}", "archive", "text.txt", "random.bin")
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(self.root / "archive.zip") as opened:
                self.assertEqual(opened.read("text.txt"), content)
                self.assertEqual(opened.getinfo("text.txt").compress_type, zipfile.ZIP_DEFLATED)
                self.assertEqual(opened.read("random.bin"), (self.root / "random.bin").read_bytes())
                self.assertEqual(opened.getinfo("random.bin").compress_type, zipfile.ZIP_STORED)

    def test_large_fast_file_uses_bounded_memory_path(self):
        large = self.root / "large.bin"
        with large.open("wb") as output:
            output.truncate(65 * 1024 * 1024)
        result = self.run_katzip("-1", "archive", "large.bin")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as opened:
            info = opened.getinfo("large.bin")
            self.assertEqual(info.file_size, large.stat().st_size)
            self.assertEqual((self.root / "archive.zip").stat().st_size - info.compress_size,
                             98 + 2 * len("large.bin"))
            with opened.open("large.bin") as data:
                while data.read(1024 * 1024):
                    pass
            self.assertIsNone(opened.testzip())

    def test_unsafe_config_is_rejected(self):
        (self.root / "input.txt").write_text("test")
        config = self.root / "custom.ini"
        environment = dict(os.environ, KATZIP_INI=str(config))
        for key, replacement in (
            ("i_maximum_block_size = 1000000", "i_maximum_block_size = 1000001"),
            ("i_min_start_fp = -6", "i_min_start_fp = -2147483648"),
        ):
            config.write_text(CONFIG.read_text().replace(key, replacement))
            result = self.run_katzip("-9", "archive", "input.txt", env=environment)
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
        result = self.run_katzip("-r", "-1", "selection", "report.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "selection.zip") as opened:
            self.assertEqual(opened.namelist(), ["report.txt"])

    def test_path_mask_uses_root_relative_posix_glob(self):
        (self.root / "src" / "nested").mkdir(parents=True)
        (self.root / "src" / "nested" / "file1.txt").write_text("one")
        (self.root / "src" / "nested" / "fileA.txt").write_text("other")
        (self.root / "src" / "file2.txt").write_text("top")
        result = self.run_katzip("-r", "-1", "selection", "src", "@nested/file[0-9].txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "selection.zip") as opened:
            self.assertEqual(opened.namelist(), ["src/nested/file1.txt"])


if __name__ == "__main__":
    unittest.main()

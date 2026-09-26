import os
import pathlib
import resource
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import unittest
import zipfile


PROGRAM = pathlib.Path(__file__).resolve().parents[1] / "katzip"


class KatzipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def run_katzip(self, *arguments, **kwargs):
        return subprocess.run(
            [str(PROGRAM), *arguments], cwd=self.root, capture_output=True, **kwargs
        )

    def default_ini(self):
        binary_dir = self.root / "defaults"
        binary_dir.mkdir()
        binary = binary_dir / "katzip"
        shutil.copy2(PROGRAM, binary)
        (binary_dir / "input.txt").write_text("default settings")
        environment = dict(os.environ)
        environment.pop("KATZIP_INI", None)
        result = subprocess.run(
            [str(binary), "-1", "archive", "input.txt"],
            cwd=binary_dir, env=environment, capture_output=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return (binary_dir / "katzip.ini").read_text()

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

    def test_invalid_utf8_filename_has_no_utf8_flag(self):
        name = b"invalid-\xe0\x80\x80.txt"
        path = os.fsencode(self.root) + b"/" + name
        with open(path, "wb") as output:
            output.write(b"text")
        result = subprocess.run(
            [os.fsencode(PROGRAM), b"-1", b"archive", name],
            cwd=self.root, capture_output=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as archive:
            info = archive.infolist()[0]
            self.assertEqual(info.flag_bits & 0x0800, 0)
            self.assertEqual(archive.read(info), b"text")

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
        result = self.run_katzip("--print-default-ini")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unknown option", result.stderr)

    def test_options_can_follow_archive_and_inputs(self):
        (self.root / "one.txt").write_text("one " * 1000)
        (self.root / "two.txt").write_text("two")
        (self.root / "-dash.txt").write_text("dash")
        (self.root / "nested").mkdir()
        (self.root / "nested" / "deep.txt").write_text("deep")

        result = self.run_katzip(
            "mixed", "one.txt", "-1", "two.txt", "--", "-dash.txt"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "mixed.zip") as archive:
            self.assertEqual(archive.namelist(),
                             ["one.txt", "two.txt", "-dash.txt"])
            self.assertEqual(archive.getinfo("one.txt").flag_bits & 0x0006,
                             0x0006)

        result = self.run_katzip(
            "recursive", "nested", "@*.txt", "-r", "-1"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "recursive.zip") as archive:
            self.assertEqual(archive.namelist(), ["nested/deep.txt"])

        result = self.run_katzip("--", "-archive", "-dash.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "-archive.zip") as archive:
            self.assertEqual(archive.read("-dash.txt"), b"dash")

        result = self.run_katzip("invalid", "one.txt", "-unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"unknown option", result.stderr)
        self.assertFalse((self.root / "invalid.zip").exists())

        result = self.run_katzip("help-only", "one.txt", "--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"Usage:", result.stdout)
        self.assertFalse((self.root / "help-only.zip").exists())

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

    def test_interrupt_removes_temporary_archive(self):
        archive = self.root / "archive.zip"
        archive.write_bytes(b"previous archive")
        (self.root / "large.bin").write_bytes(os.urandom(262144))
        for interrupt in (signal.SIGINT, signal.SIGTERM):
            process = subprocess.Popen(
                [str(PROGRAM), "-9", "archive", "large.bin"],
                cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.PIPE
            )
            try:
                deadline = time.monotonic() + 10
                while not list(self.root.glob("archive.zip.tmp.*")) and time.monotonic() < deadline:
                    self.assertIsNone(process.poll(), "katzip exited before creating its temporary archive")
                    time.sleep(0.01)
                self.assertTrue(list(self.root.glob("archive.zip.tmp.*")))
                process.send_signal(interrupt)
                process.communicate(timeout=10)
                self.assertEqual(process.returncode, 128 + interrupt)
                self.assertEqual(archive.read_bytes(), b"previous archive")
                self.assertEqual(list(self.root.glob("archive.zip.tmp.*")), [])
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

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

    def test_ect_levels_handle_empty_and_incompressible_files(self):
        (self.root / "empty.bin").write_bytes(b"")
        random_data = os.urandom(4096)
        (self.root / "random.bin").write_bytes(random_data)
        for level in (7, 8):
            result = self.run_katzip(
                f"-{level}", "archive", "empty.bin", "random.bin"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(self.root / "archive.zip") as archive:
                self.assertEqual(archive.read("empty.bin"), b"")
                self.assertEqual(archive.read("random.bin"), random_data)
                self.assertEqual(
                    archive.getinfo("random.bin").compress_type,
                    zipfile.ZIP_STORED,
                )
                self.assertIsNone(archive.testzip())

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
        defaults = self.default_ini()
        for key, replacement in (
            ("turtledeflate_i_maximum_block_size = 1000000",
             "turtledeflate_i_maximum_block_size = 1000001"),
            ("turtledeflate_i_min_start_fp = -6",
             "turtledeflate_i_min_start_fp = -2147483648"),
        ):
            config.write_text(defaults.replace(key, replacement))
            result = self.run_katzip("-9", "archive", "input.txt", env=environment)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid settings", result.stderr)
            self.assertFalse((self.root / "archive.zip").exists())

    def test_ect_settings_are_required_and_checked(self):
        (self.root / "input.txt").write_text("A repeated sentence. " * 100)
        config = self.root / "custom.ini"
        environment = dict(os.environ, KATZIP_INI=str(config))
        defaults = self.default_ini()
        for level in (7, 8, 9):
            section = f"[{level}]\n"
            head, rest = defaults.split(section, 1)
            body, tail = rest.split("\n\n", 1) if level < 9 else (rest, "")
            body = body.replace("zopfli_numiterations = ",
                                "removed_numiterations = ", 1)
            config.write_text(head + section + body +
                              ("\n\n" + tail if tail else ""))
            result = self.run_katzip(
                f"-{level}", "archive", "input.txt", env=environment
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid setting", result.stderr)
        for replacement in ("zopfli_numiterations = 0", "zopfli_numiterations = 1001"):
            config.write_text(defaults.replace("zopfli_numiterations = 13", replacement, 1))
            result = self.run_katzip(
                "-7", "archive", "input.txt", env=environment
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid setting", result.stderr)
        config.write_text(defaults.replace("zopfli_numiterations = 13",
                                           "zopfli_numiterations = 1", 1))
        result = self.run_katzip("-7", "archive", "input.txt", env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as archive:
            self.assertEqual(archive.read("input.txt"),
                             (self.root / "input.txt").read_bytes())

    def test_size_threshold_and_compressor_selection(self):
        content = b"abc xyz abc xyz " * 500
        (self.root / "input.txt").write_bytes(content)
        defaults = self.default_ini()
        config = self.root / "custom.ini"
        environment = dict(os.environ, KATZIP_INI=str(config))
        start = defaults.index("[7]\n")
        end = defaults.index("[8]\n", start)
        base = defaults[start:end]
        for threshold in ("off", "1KiB", "8192B", "0"):
            section = base.replace("zlib_after = off", f"zlib_after = {threshold}")
            config.write_text(defaults[:start] + section + defaults[end:])
            result = self.run_katzip("-7", "archive", "input.txt", env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(self.root / "archive.zip") as archive:
                self.assertEqual(archive.read("input.txt"), content)
                if threshold in ("1KiB", "0"):
                    selected = archive.getinfo("input.txt").compress_size
                    if threshold == "1KiB":
                        switched_size = selected
                    else:
                        self.assertEqual(selected, switched_size)
        config.write_text(defaults[:start] +
                          base.replace("zlib_after = off", "zlib_after = 1*1024") +
                          defaults[end:])
        result = self.run_katzip("-7", "archive", "input.txt", env=environment)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"invalid setting", result.stderr)

    def test_multiple_compressors_compete_without_extra_zip_data(self):
        content = (b"Text with repeated phrases and changing numbers. " * 80)
        (self.root / "input.txt").write_bytes(content)
        defaults = self.default_ini()
        config = self.root / "custom.ini"
        environment = dict(os.environ, KATZIP_INI=str(config))
        start = defaults.index("[7]\n")
        end = defaults.index("[8]\n", start)
        section = defaults[start:end]
        candidates = []
        for extra in ("", "libdeflate_level = 1\n"):
            config.write_text(defaults[:start] + section.replace("[7]\n", "[7]\n" + extra, 1) + defaults[end:])
            result = self.run_katzip("-7", "archive", "input.txt", env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(self.root / "archive.zip") as archive:
                self.assertEqual(archive.read("input.txt"), content)
                candidates.append(archive.getinfo("input.txt").compress_size)
        self.assertLessEqual(candidates[1], candidates[0])
        self.assertEqual((self.root / "archive.zip").stat().st_size - candidates[1],
                         98 + 2 * len("input.txt"))
        turtle = defaults.split("[9]\n", 1)[1].split("turtledeflate_", 1)[1]
        turtle = "turtledeflate_" + turtle.split("zlib_after =", 1)[0]
        config.write_text(defaults[:start] +
                          section.replace("[7]\n", "[7]\nlibdeflate_level = 1\n" + turtle, 1) +
                          defaults[end:])
        result = self.run_katzip("-7", "archive", "input.txt", env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "archive.zip") as archive:
            self.assertEqual(archive.read("input.txt"), content)
            self.assertLessEqual(archive.getinfo("input.txt").compress_size,
                                 candidates[1])

    def test_turtle_only_and_zlib_only_sections(self):
        content = b"Separate compressor settings. " * 120
        (self.root / "input.txt").write_bytes(content)
        defaults = self.default_ini()
        config = self.root / "custom.ini"
        environment = dict(os.environ, KATZIP_INI=str(config))
        head, level_nine = defaults.split("[9]\n", 1)
        turtle_only = "\n".join(
            line for line in level_nine.splitlines()
            if not line.startswith("zopfli_")
        ) + "\n"
        config.write_text(head + "[9]\n" + turtle_only)
        result = self.run_katzip("-9", "turtle", "input.txt", env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "turtle.zip") as archive:
            self.assertEqual(archive.read("input.txt"), content)
        zlib_only = "\n".join(
            line for line in turtle_only.splitlines()
            if not line.startswith("turtledeflate_")
        ).replace("zlib_after = off", "zlib_after = 0") + "\n"
        config.write_text(head + "[9]\n" + zlib_only)
        result = self.run_katzip("-9", "zlib", "input.txt", env=environment)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "zlib.zip") as archive:
            self.assertEqual(archive.read("input.txt"), content)

    def test_invalid_ini_line_after_valid_settings_is_rejected(self):
        (self.root / "input.txt").write_text("data")
        config = self.root / "broken.ini"
        defaults = self.default_ini()
        environment = dict(os.environ, KATZIP_INI=str(config))
        broken_configs = (
            defaults.replace("[2]", "bad = 1\n[2]", 1),
            defaults + "[1]\nlibdeflate_level = 1",
        )
        for content in broken_configs:
            config.write_text(content)
            result = self.run_katzip(
                "-1", "archive", "input.txt", env=environment
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"invalid setting", result.stderr)
            self.assertFalse((self.root / "archive.zip").exists())

    def test_ini_search_order_and_automatic_generation(self):
        binary_dir = self.root / "bin"
        document_dir = self.root / "documents"
        binary_dir.mkdir()
        document_dir.mkdir()
        shutil.copy2(PROGRAM, binary_dir / "katzip")
        (document_dir / "input.txt").write_text("Repeated input. " * 100)
        defaults = self.default_ini()
        self.assertIn("[7]\nzopfli_numiterations = 13", defaults)
        self.assertIn("[8]\nzopfli_numiterations = 60", defaults)
        self.assertIn("[9]\nzopfli_numiterations = 60", defaults)
        executable_ini = binary_dir / "katzip.ini"
        current_ini = document_dir / "katzip.ini"
        executable_ini.write_text(defaults)
        current_ini.write_text(defaults.replace("libdeflate_level = 1",
                                                "libdeflate_level = 13", 1))
        environment = dict(os.environ)
        environment.pop("KATZIP_INI", None)
        environment["PATH"] = str(binary_dir) + os.pathsep + environment.get("PATH", "")

        def run(level):
            return subprocess.run(
                ["katzip", f"-{level}", "archive", "input.txt"],
                cwd=document_dir, env=environment, capture_output=True
            )

        result = run(1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"invalid setting in katzip.ini", result.stderr)
        self.assertFalse((document_dir / "archive.zip").exists())

        current_ini.unlink()
        result = run(1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"warning", result.stderr)
        with zipfile.ZipFile(document_dir / "archive.zip") as archive:
            self.assertEqual(archive.read("input.txt"), (document_dir / "input.txt").read_bytes())

        executable_ini.unlink()
        for level in range(1, 10):
            result = run(level)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(b"warning", result.stderr)
            self.assertEqual(executable_ini.read_text(), defaults)
            self.assertEqual(list(binary_dir.glob("katzip.ini.tmp.*")), [])
            self.assertFalse(current_ini.exists())
            with zipfile.ZipFile(document_dir / "archive.zip") as archive:
                self.assertEqual(archive.read("input.txt"), (document_dir / "input.txt").read_bytes())

        executable_ini.unlink()
        environment["KATZIP_INI"] = str(executable_ini)
        result = run(1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"cannot open", result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "root can write to read-only directories")
    def test_read_only_binary_directory_uses_compiled_settings(self):
        binary_dir = self.root / "bin"
        document_dir = self.root / "documents"
        binary_dir.mkdir()
        document_dir.mkdir()
        shutil.copy2(PROGRAM, binary_dir / "katzip")
        (document_dir / "input.txt").write_text("data " * 100)
        binary_dir.chmod(0o555)
        environment = dict(os.environ)
        environment.pop("KATZIP_INI", None)
        try:
            for level in (1, 7, 8):
                result = subprocess.run(
                    [str(binary_dir / "katzip"), f"-{level}", "archive", "input.txt"],
                    cwd=document_dir, env=environment, capture_output=True
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(b"using built-in compression settings", result.stderr)
                with zipfile.ZipFile(document_dir / "archive.zip") as archive:
                    self.assertEqual(archive.read("input.txt"),
                                     (document_dir / "input.txt").read_bytes())
        finally:
            binary_dir.chmod(0o755)
        self.assertFalse((binary_dir / "katzip.ini").exists())
        self.assertFalse((document_dir / "katzip.ini").exists())

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

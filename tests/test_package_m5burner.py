import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class M5BurnerPackageTests(unittest.TestCase):
    def make_fixture(self, temp: str, *, core_version="3.3.11") -> tuple[Path, dict]:
        fixture = Path(temp)
        (fixture / "tools").mkdir()
        (fixture / "docs").mkdir()
        (fixture / "build").mkdir()
        (fixture / "fake-bin").mkdir()
        shutil.copy(ROOT / "tools" / "package-m5burner.sh", fixture / "tools")
        shutil.copy(ROOT / "tools" / "validate-release.mjs", fixture / "tools")
        shutil.copy(ROOT / "tools" / "validate-images.mjs", fixture / "tools")
        shutil.copy(ROOT / "tools" / "arduino-cli.yaml", fixture / "tools")
        shutil.copy(ROOT / "VERSION", fixture)
        shutil.copy(ROOT / "README.md", fixture)
        shutil.copy(ROOT / "m5burner.json", fixture)
        shutil.copy(ROOT / "docs" / "RELEASE_NOTES.md", fixture / "docs")

        partition_entries = b"".join(
            struct.pack("<HBBII16sI", 0x50AA, 1, 0, offset, size, label.encode(), 0)
            for label, offset, size in (
                ("nvs", 0x9000, 0x5000),
                ("otadata", 0xE000, 0x2000),
                ("app0", 0x10000, 0x330000),
                ("app1", 0x340000, 0x330000),
                ("spiffs", 0x670000, 0x180000),
                ("coredump", 0x7F0000, 0x10000),
            )
        )
        partition_table = partition_entries + struct.pack(
            "<H14s16s", 0xEBEB, b"\xff" * 14, hashlib.md5(partition_entries).digest()
        )
        partition_table += b"\xff" * (3072 - len(partition_table))
        sources = {
            "hotspot-arcade-cardputer.ino.bootloader.bin": b"bootloader",
            "hotspot-arcade-cardputer.ino.partitions.bin": partition_table,
            "hotspot-arcade-cardputer.ino.bin": b"application",
        }
        for name, content in sources.items():
            (fixture / "build" / name).write_bytes(content)

        data_dir = fixture / "arduino-data"
        boot_app = (
            data_dir
            / "packages"
            / "esp32"
            / "hardware"
            / "esp32"
            / core_version
            / "tools"
            / "partitions"
            / "boot_app0.bin"
        )
        boot_app.parent.mkdir(parents=True)
        boot_app.write_bytes(b"boot-app-zero")
        lock = json.loads((ROOT / "tools" / "toolchain.lock.json").read_text())
        lock["arduino"]["bootApp0"] = {
            "relativeToData": str(boot_app.relative_to(data_dir)),
            "size": boot_app.stat().st_size,
            "sha256": hashlib.sha256(boot_app.read_bytes()).hexdigest(),
        }
        (fixture / "tools" / "toolchain.lock.json").write_text(json.dumps(lock))

        fake_cli = fixture / "fake-bin" / "arduino-cli"
        fake_cli.write_text(
            "#!/usr/bin/env bash\n"
            "if [[ \"$1\" == \"--config-file\" ]]; then shift 2; fi\n"
            "if [[ \"$1 $2 $3\" == \"config get directories.data\" ]]; then\n"
            "  printf '%s\\n' \"$FAKE_ARDUINO_DATA\"\n"
            "elif [[ \"$1 $2\" == \"core list\" ]]; then\n"
            "  printf 'ID Installed Latest Name\\nesp32:esp32 %s %s esp32\\n' "
            "\"$FAKE_CORE_VERSION\" \"$FAKE_CORE_VERSION\"\n"
            "else\n"
            "  exit 2\n"
            "fi\n",
            encoding="utf-8",
        )
        fake_cli.chmod(0o755)
        fake_esptool = fixture / "fake-bin" / "esptool"
        fake_esptool.write_text(
            "#!/usr/bin/env bash\n"
            "if [[ \"$1\" == \"version\" ]]; then printf 'esptool v5.3.1\\n'; exit 0; fi\n"
            "if [[ \"$1\" == \"image-info\" ]]; then\n"
            "  printf 'Detected image type: ESP32-S3\\nFlash size: 8MB\\nChecksum: valid (valid)\\n'\n"
            "  exit 0\n"
            "fi\n"
            "exit 2\n",
            encoding="utf-8",
        )
        fake_esptool.chmod(0o755)
        environment = os.environ.copy()
        environment["FAKE_ARDUINO_DATA"] = str(data_dir)
        environment["FAKE_CORE_VERSION"] = core_version
        environment["PATH"] = f"{fixture / 'fake-bin'}:{environment['PATH']}"
        return fixture, environment

    def test_packages_only_the_locked_core_boot_app(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture, environment = self.make_fixture(temp)
            result = subprocess.run(
                ["bash", "tools/package-m5burner.sh"],
                cwd=fixture,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            output = fixture / "firmware" / "cardputer"
            self.assertEqual((output / "bootloader_0x0.bin").read_bytes(), b"bootloader")
            self.assertEqual((output / "partitions_0x8000.bin").stat().st_size, 3072)
            self.assertTrue((output / "partitions_0x8000.bin").read_bytes().startswith(b"\xaaP"))
            self.assertEqual((output / "boot_app0_0xe000.bin").read_bytes(), b"boot-app-zero")
            self.assertEqual((output / "hotspot-arcade_0x10000.bin").read_bytes(), b"application")
            self.assertTrue((fixture / "build" / "hotspot-arcade-cardputer-m5burner.zip").is_file())

    def test_rejects_a_different_installed_core(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture, environment = self.make_fixture(temp, core_version="3.4.0")
            result = subprocess.run(
                ["bash", "tools/package-m5burner.sh"],
                cwd=fixture,
                env=environment,
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("esp32:esp32@3.3.11", result.stderr)


if __name__ == "__main__":
    unittest.main()

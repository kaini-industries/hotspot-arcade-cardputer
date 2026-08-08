#!/usr/bin/env python3
"""Validate the exact, deterministic M5Burner component directory and ZIP."""

from __future__ import annotations

import argparse
import stat
import sys
import zipfile
from pathlib import Path


EXPECTED_TIMESTAMP = (1980, 1, 1, 0, 0, 0)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--components", required=True, type=Path)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--boot-app", required=True, type=Path)
    return parser.parse_args()


def validate_package(
    build: Path, components: Path, archive: Path, boot_app: Path
) -> None:
    expected = [
        ("bootloader_0x0.bin", build / "hotspot-arcade-cardputer.ino.bootloader.bin", 0x0, 0x8000),
        ("partitions_0x8000.bin", build / "hotspot-arcade-cardputer.ino.partitions.bin", 0x8000, 0xE000),
        ("boot_app0_0xe000.bin", boot_app, 0xE000, 0x10000),
        ("hotspot-arcade_0x10000.bin", build / "hotspot-arcade-cardputer.ino.bin", 0x10000, 0x340000),
    ]
    expected_names = [entry[0] for entry in expected]

    if not components.is_dir() or components.is_symlink():
        raise ValueError(f"component path is not a real directory: {components}")
    actual_names = sorted(entry.name for entry in components.iterdir())
    if actual_names != sorted(expected_names):
        raise ValueError(
            f"M5Burner component directory must contain exactly {expected_names}; found {actual_names}"
        )

    source_bytes: dict[str, bytes] = {}
    for name, source, offset, limit in expected:
        if not source.is_file() or source.is_symlink():
            raise ValueError(f"source image is not a regular file: {source}")
        data = source.read_bytes()
        if not data:
            raise ValueError(f"source image is empty: {source}")
        if offset + len(data) > limit:
            raise ValueError(
                f"{name} exceeds its fixed region: 0x{offset:x} + {len(data)} > 0x{limit:x}"
            )
        component = components / name
        if component.is_symlink() or not component.is_file():
            raise ValueError(f"component is not a regular file: {component}")
        if component.read_bytes() != data:
            raise ValueError(f"component contents do not match the validated source: {name}")
        source_bytes[name] = data

    if not archive.is_file() or archive.is_symlink() or archive.stat().st_size == 0:
        raise ValueError(f"archive is not a nonempty regular file: {archive}")
    try:
        with zipfile.ZipFile(archive) as package:
            if package.comment:
                raise ValueError("M5Burner archive comment must be empty")
            entries = package.infolist()
            names = [entry.filename for entry in entries]
            if names != expected_names:
                raise ValueError(
                    f"M5Burner archive entries/order must be exactly {expected_names}; found {names}"
                )
            if len(set(names)) != len(names):
                raise ValueError("M5Burner archive contains duplicate entries")
            for entry in entries:
                mode = entry.external_attr >> 16
                if entry.flag_bits & 0x1:
                    raise ValueError(f"encrypted M5Burner entry is forbidden: {entry.filename}")
                if entry.date_time != EXPECTED_TIMESTAMP:
                    raise ValueError(f"non-deterministic ZIP timestamp for {entry.filename}")
                if entry.extra or entry.comment:
                    raise ValueError(f"ZIP metadata is not stripped for {entry.filename}")
                if entry.create_system != 3 or not stat.S_ISREG(mode) or stat.S_IMODE(mode) != 0o644:
                    raise ValueError(f"unsafe or non-deterministic ZIP mode for {entry.filename}")
                if entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    raise ValueError(f"unexpected compression method for {entry.filename}")
                if package.read(entry) != source_bytes[entry.filename]:
                    raise ValueError(f"archived component does not match source: {entry.filename}")
    except zipfile.BadZipFile as exc:
        raise ValueError(f"invalid M5Burner ZIP: {archive}") from exc


def main() -> int:
    args = _arguments()
    try:
        validate_package(args.build, args.components, args.archive, args.boot_app)
    except (OSError, ValueError) as exc:
        print(f"M5Burner package validation failed: {exc}", file=sys.stderr)
        return 1
    print("validated deterministic M5Burner ZIP contents and fixed component offsets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

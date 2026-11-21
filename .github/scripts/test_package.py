#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Local fixture checks for the package layout and archive helpers."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import package
import version


class PackageFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="lancedb-c-test-")
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        (self.repo / "include").mkdir(parents=True)
        (self.repo / "LICENSE").write_text("license\n", encoding="utf-8")
        (self.repo / "include" / "lancedb.h").write_text("header\n", encoding="utf-8")
        self.library_dir = self.root / "library"
        self.library_dir.mkdir()
        (self.library_dir / "liblancedb.so").write_bytes(b"fake shared library")

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def create(self, output_name: str) -> tuple[Path, Path]:
        output = self.root / output_name
        args = argparse.Namespace(
            target="x86_64-unknown-linux-gnu",
            commit="abc123",
            repo_root=str(self.repo),
            library_dir=str(self.library_dir),
            output_dir=str(output),
        )
        with contextlib.redirect_stdout(io.StringIO()):
            with mock.patch.dict(os.environ, {"LANCEDB_VERSION": "0.1.0-preview.1"}):
                package.create_package(args)
        archive = output / "lancedb-c-abc123-linux-amd64.tar.gz"
        return archive, Path(f"{archive}.sha256")


class VersionPolicyTests(unittest.TestCase):
    def test_valid_release_tags(self) -> None:
        for tag, expected in {
            "v0.1.0": "0.1.0",
            "v0.1.0-preview.1": "0.1.0-preview.1",
            "v0.1.0-0.a-2+build.01": "0.1.0-0.a-2+build.01",
        }.items():
            self.assertEqual(version.validate_tag(tag), expected)

    def test_invalid_release_tags(self) -> None:
        for tag in (
            "v01.2.3",
            "v0.1.0-preview.01",
            "v0.0.0-dev",
            "v1.2",
            "v0.1.0-",
            "v0.1.0+",
            "v0.1.0-a..b",
            "1.2.3",
        ):
            with self.assertRaises(ValueError):
                version.validate_tag(tag)

    def test_override_must_be_strict_without_v_prefix(self) -> None:
        self.assertEqual(version.parse_override("0.1.0-preview.1"), "0.1.0-preview.1")
        for value in ("v0.1.0", "0.0.0-dev", "01.2.3", "0.1.0-preview.01"):
            with self.assertRaises(ValueError):
                version.parse_override(value)


class VersionTests(PackageFixture):
    def init_repo(self, name: str) -> Path:
        repo = self.root / name
        repo.mkdir()
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.email", "test@example.com"],
            check=True,
        )
        subprocess.run(
            ["git", "-C", str(repo), "config", "user.name", "Test"], check=True
        )
        (repo / "tracked").write_text("one\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(repo), "add", "tracked"], check=True)
        subprocess.run(["git", "-C", str(repo), "commit", "-qm", "one"], check=True)
        return repo

    def test_package_version_uses_exact_annotated_tag(self) -> None:
        repo = self.init_repo("tagged")
        subprocess.run(
            ["git", "-C", str(repo), "tag", "-a", "v0.1.0-preview.1", "-m", "release"],
            check=True,
        )

        with mock.patch.dict(os.environ):
            os.environ.pop("LANCEDB_VERSION", None)
            self.assertEqual(package.package_version(repo), "0.1.0-preview.1")

            (repo / "tracked").write_text("two\n", encoding="utf-8")
            subprocess.run(
                ["git", "-C", str(repo), "commit", "-qam", "two"], check=True
            )
            self.assertEqual(package.package_version(repo), package.DEV_VERSION)

            os.environ["LANCEDB_VERSION"] = "0.2.0-preview.3"
            self.assertEqual(package.package_version(repo), "0.2.0-preview.3")

    def test_multiple_valid_tags_prefer_stable(self) -> None:
        repo = self.init_repo("multiple")
        subprocess.run(["git", "-C", str(repo), "tag", "v0.1.0"], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "tag", "v0.2.0-preview.2"], check=True
        )

        with mock.patch.dict(os.environ):
            os.environ.pop("LANCEDB_VERSION", None)
            self.assertEqual(package.package_version(repo), "0.1.0")

    def test_invalid_and_development_tags_do_not_hide_valid_tag(self) -> None:
        repo = self.init_repo("mixed")
        subprocess.run(["git", "-C", str(repo), "tag", "v0.1.0"], check=True)
        subprocess.run(["git", "-C", str(repo), "tag", "v01.2.3"], check=True)
        subprocess.run(
            ["git", "-C", str(repo), "tag", "v0.1.0-preview.01"], check=True
        )
        subprocess.run(["git", "-C", str(repo), "tag", "v0.0.0-dev"], check=True)
        subprocess.run(
            [
                "git",
                "-C",
                str(repo),
                "tag",
                "-a",
                "v0.1.0-not_a_version",
                "-m",
                "not a release",
            ],
            check=True,
        )

        with mock.patch.dict(os.environ):
            os.environ.pop("LANCEDB_VERSION", None)
            self.assertEqual(package.package_version(repo), "0.1.0")

    def test_private_resolved_version_precedes_public_override(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "LANCEDB_RESOLVED_VERSION": "0.1.0-preview.1",
                "LANCEDB_VERSION": "0.9.0",
            },
        ):
            self.assertEqual(package.package_version(self.repo), "0.1.0-preview.1")

    def test_private_resolved_dev_is_allowed(self) -> None:
        with mock.patch.dict(os.environ, {"LANCEDB_RESOLVED_VERSION": package.DEV_VERSION}):
            self.assertEqual(package.package_version(self.repo), package.DEV_VERSION)

    def test_private_resolved_version_rejects_invalid(self) -> None:
        for value in ("v0.1.0", "01.2.3", "0.1.0-preview.01"):
            with mock.patch.dict(os.environ, {"LANCEDB_RESOLVED_VERSION": value}):
                with self.assertRaises(SystemExit):
                    package.package_version(self.repo)

    def test_package_version_ignores_parent_repository_tag(self) -> None:
        parent = self.init_repo("parent")
        subprocess.run(
            ["git", "-C", str(parent), "tag", "-a", "v9.9.9", "-m", "parent"],
            check=True,
        )
        source = parent / "source"
        source.mkdir()

        with mock.patch.dict(os.environ):
            os.environ.pop("LANCEDB_VERSION", None)
            self.assertEqual(package.package_version(source), package.DEV_VERSION)

    def test_package_version_rejects_invalid_override(self) -> None:
        for value in ("v1", "v0.1.0", "0.0.0-dev", "01.2.3"):
            with mock.patch.dict(os.environ, {"LANCEDB_VERSION": value}):
                with self.assertRaises(SystemExit):
                    package.package_version(self.repo)


class PackageArchiveTests(PackageFixture):
    def create_with_umask(self, output_name: str, umask: int, timestamp: int) -> tuple[Path, Path]:
        previous_umask = os.umask(umask)
        try:
            for path in (
                self.repo / "LICENSE",
                self.repo / "include" / "lancedb.h",
                self.library_dir / "liblancedb.so",
            ):
                os.utime(path, (timestamp, timestamp))
            with mock.patch("time.time", return_value=timestamp):
                return self.create(output_name)
        finally:
            os.umask(previous_umask)

    def test_identical_inputs_produce_identical_archives(self) -> None:
        first, first_checksum = self.create_with_umask("first", 0o022, 1_000_000_000)
        second, second_checksum = self.create_with_umask("second", 0o077, 1_700_000_000)

        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(
            first_checksum.read_text(encoding="utf-8"),
            second_checksum.read_text(encoding="utf-8"),
        )

        gzip_header = first.read_bytes()[:10]
        self.assertEqual(gzip_header[:2], b"\x1f\x8b")
        self.assertEqual(int.from_bytes(gzip_header[4:8], "little"), 0)

        with tarfile.open(first, "r:gz") as tar:
            modes = {
                member.name.split("/", 1)[1]: member.mode
                for member in tar.getmembers()
                if "/" in member.name
            }
        self.assertEqual(modes["include"], 0o755)
        self.assertEqual(modes["lib"], 0o755)
        self.assertEqual(modes["LICENSE"], 0o644)
        self.assertEqual(modes["include/lancedb.h"], 0o644)
        self.assertEqual(modes["lib/liblancedb.so"], 0o755)
        self.assertEqual(modes["manifest.json"], 0o644)
        self.assertEqual(modes["SHA256SUMS"], 0o644)

    def test_manifest_records_package_version(self) -> None:
        archive, _ = self.create("package")
        with tempfile.TemporaryDirectory(prefix="lancedb-c-extract-") as temp:
            package.extract_tar(archive, Path(temp))
            manifest = json.loads(
                (Path(temp) / "lancedb-c-abc123-linux-amd64" / "manifest.json").read_text(
                    encoding="utf-8"
                )
            )

        self.assertEqual(manifest["version"], "0.1.0-preview.1")

    def test_package_version_defaults_to_dev_without_git(self) -> None:
        with mock.patch.dict(os.environ):
            os.environ.pop("LANCEDB_VERSION", None)
            self.assertEqual(package.package_version(self.repo), package.DEV_VERSION)

    def test_archive_checksum_is_required_and_matches(self) -> None:
        archive, checksum = self.create("package")
        digest = package.verify_archive_checksum(archive, checksum)

        self.assertEqual(digest, package.sha256_file(archive))
        with self.assertRaises(SystemExit):
            package.verify_archive_checksum(archive, self.root / "missing.sha256")
        bad_name = self.root / "bad-name.sha256"
        bad_name.write_text(f"{digest}  wrong.tar.gz\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            package.verify_archive_checksum(archive, bad_name)
        bad_digest = self.root / "bad-digest.sha256"
        bad_digest.write_text(f"{'0' * 64}  {archive.name}\n", encoding="utf-8")
        with self.assertRaises(SystemExit):
            package.verify_archive_checksum(archive, bad_digest)

    def test_checksum_inventory_is_complete_and_unique(self) -> None:
        archive, _ = self.create("package")
        with tempfile.TemporaryDirectory(prefix="lancedb-c-extract-") as temp:
            package.extract_tar(archive, Path(temp))
            package_root = Path(temp) / "lancedb-c-abc123-linux-amd64"

            package.verify_checksums(package_root)
            inventory = package.read_checksum_inventory(package_root)
            self.assertEqual(
                set(inventory),
                {
                    "LICENSE",
                    "include/lancedb.h",
                    "lib/liblancedb.so",
                    "manifest.json",
                },
            )

            checksum_file = package_root / "SHA256SUMS"
            original = checksum_file.read_text(encoding="utf-8")
            lines = original.splitlines()

            checksum_file.write_text(original + lines[0] + "\n", encoding="utf-8")
            with self.assertRaisesRegex(SystemExit, "more than once"):
                package.read_checksum_inventory(package_root)

            checksum_file.write_text(
                "\n".join(line for line in lines if not line.endswith("  manifest.json")) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SystemExit, "SHA256SUMS is missing: manifest\\.json"):
                package.verify_checksums(package_root)

            checksum_file.write_text(
                original + f"{'0' * 64}  unexpected.txt\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(
                SystemExit, "SHA256SUMS lists unexpected entries: unexpected\\.txt"
            ):
                package.verify_checksums(package_root)


class ParserTests(unittest.TestCase):
    def test_parse_macho_install_name(self) -> None:
        output = """\
liblancedb.dylib:
Load command 0
      cmd LC_ID_DYLIB
  cmdsize 48
     name @rpath/liblancedb.dylib (offset 24)
"""
        self.assertEqual(
            package.parse_macho_install_name(output), "@rpath/liblancedb.dylib"
        )
        self.assertIsNone(package.parse_macho_install_name("liblancedb.dylib:\n"))

    def test_binary_arch_parsers(self) -> None:
        with tempfile.TemporaryDirectory(prefix="lancedb-c-arch-") as temp:
            root = Path(temp)

            elf = bytearray(64)
            elf[:4] = b"\x7fELF"
            elf[5] = 1
            struct.pack_into("<H", elf, 18, 0x3E)
            (root / "lib.so").write_bytes(elf)
            self.assertEqual(package.read_elf_arch(root / "lib.so"), "x86_64")

            macho = bytearray(8)
            struct.pack_into("<I", macho, 0, 0xFEEDFACF)
            struct.pack_into("<I", macho, 4, 0x0100000C)
            (root / "lib.dylib").write_bytes(macho)
            self.assertEqual(package.read_macho_arch(root / "lib.dylib"), "aarch64")

            pe = bytearray(4096)
            pe[:2] = b"MZ"
            struct.pack_into("<I", pe, 0x3C, 0x80)
            pe[0x80:0x84] = b"PE\x00\x00"
            struct.pack_into("<H", pe, 0x84, 0x8664)
            (root / "lib.dll").write_bytes(pe)
            self.assertEqual(package.read_pe_arch(root / "lib.dll"), "x86_64")


if __name__ == "__main__":
    unittest.main(verbosity=2)

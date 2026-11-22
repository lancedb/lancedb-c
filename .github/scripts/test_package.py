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
import re
import shutil
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
        (self.repo / "include" / "lancedb.h").write_text(
            package.HEADER_VERSION_DEFAULT + "\n", encoding="utf-8"
        )
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
        for value in (
            "v0.1.0",
            "0.0.0-dev",
            "01.2.3",
            "0.1.0-preview.01",
            " 0.1.0",
            "0.1.0 ",
        ):
            with self.assertRaises(ValueError):
                version.parse_override(value)

    def test_resolved_version_accepts_dev_and_strict(self) -> None:
        self.assertEqual(version.parse_resolved("0.0.0-dev"), "0.0.0-dev")
        self.assertEqual(version.parse_resolved("0.1.0-preview.1"), "0.1.0-preview.1")

    def test_resolved_version_rejects_invalid(self) -> None:
        for value in ("v0.1.0", "01.2.3", "0.1.0-preview.01", " 0.1.0"):
            with self.assertRaises(ValueError):
                version.parse_resolved(value)

    def test_soversion_tracks_the_semver_compatibility_boundary(self) -> None:
        self.assertEqual(version.soversion("0.1.0"), "0.1")
        self.assertEqual(version.soversion("0.1.0-preview.1"), "0.1")
        self.assertEqual(version.soversion("0.2.0"), "0.2")
        self.assertEqual(version.soversion("1.4.2"), "1")
        self.assertEqual(version.soversion("2.0.0-rc.1"), "2")


class HeaderRenderTests(unittest.TestCase):
    def test_renders_resolved_release_values(self) -> None:
        source = (package.HEADER_VERSION_DEFAULT + "\ntrailer\n").encode()
        rendered = package.render_header(source, "0.2.3-preview.4+build.5")
        text = rendered.decode()
        self.assertIn("#define LANCEDB_VERSION_MAJOR 0\n", text)
        self.assertIn("#define LANCEDB_VERSION_MINOR 2\n", text)
        self.assertIn("#define LANCEDB_VERSION_PATCH 3\n", text)
        self.assertIn('#define LANCEDB_VERSION_STRING "0.2.3-preview.4+build.5"\n', text)
        self.assertNotIn("0.0.0-dev", text)
        self.assertIn("trailer\n", text)
        self.assertNotIn(b"\r", rendered)

    def test_keeps_development_default(self) -> None:
        source = (package.HEADER_VERSION_DEFAULT + "\n").encode()
        self.assertEqual(package.render_header(source, version.DEV_VERSION), source)

    def test_rejects_missing_duplicate_or_crlf_markers(self) -> None:
        with self.assertRaisesRegex(SystemExit, "exactly one default"):
            package.render_header(b"header\n", version.DEV_VERSION)
        duplicated = (package.HEADER_VERSION_DEFAULT + "\n" + package.HEADER_VERSION_DEFAULT).encode()
        with self.assertRaisesRegex(SystemExit, "found 2"):
            package.render_header(duplicated, version.DEV_VERSION)
        crlf = (package.HEADER_VERSION_DEFAULT + "\n").replace("\n", "\r\n").encode()
        with self.assertRaisesRegex(SystemExit, "LF line endings"):
            package.render_header(crlf, version.DEV_VERSION)

    def test_checked_in_header_keeps_development_default(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        header = (repo_root / "include" / "lancedb.h").read_text(encoding="utf-8")
        self.assertEqual(header.count(package.HEADER_VERSION_DEFAULT), 1)
        self.assertIn(
            "#define LANCEDB_VERSION LANCEDB_VERSION_STRING\n", header
        )

    def test_rejects_invalid_version(self) -> None:
        source = (package.HEADER_VERSION_DEFAULT + "\n").encode()
        with self.assertRaises(SystemExit):
            package.render_header(source, "v0.1.0")


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

    def test_docs_only_resolved_env_does_not_override_tag(self) -> None:
        repo = self.init_repo("no-leak-tag")
        subprocess.run(["git", "-C", str(repo), "tag", "v0.1.0"], check=True)
        with mock.patch.dict(os.environ, {"LANCEDB_RESOLVED_VERSION": "0.9.0"}):
            self.assertEqual(package.package_version(repo), "0.1.0")

    def test_docs_only_resolved_env_does_not_override_public(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "LANCEDB_RESOLVED_VERSION": "0.9.0",
                "LANCEDB_VERSION": "0.2.0",
            },
        ):
            self.assertEqual(package.package_version(self.repo), "0.2.0")

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
        for value in ("v1", "v0.1.0", "0.0.0-dev", "01.2.3", " 0.1.0", "0.1.0 "):
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
        self.assertEqual(modes["lib/liblancedb.so.0.1"], 0o755)
        self.assertEqual(modes["lib/liblancedb.so"], 0o777)
        self.assertEqual(modes["manifest.json"], 0o644)
        self.assertEqual(modes["SHA256SUMS"], 0o644)

    def test_linux_package_uses_versioned_soname_and_unversioned_link(self) -> None:
        archive, _ = self.create("package")
        with tempfile.TemporaryDirectory(prefix="lancedb-c-extract-") as temp:
            package.extract_tar(archive, Path(temp))
            package_root = Path(temp) / "lancedb-c-abc123-linux-amd64"
            link = package_root / "lib" / "liblancedb.so"
            self.assertTrue(link.is_symlink())
            self.assertEqual(os.readlink(link), "liblancedb.so.0.1")
            self.assertTrue((package_root / "lib" / "liblancedb.so.0.1").is_file())

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
        with tempfile.TemporaryDirectory(prefix="lancedb-c-extract-") as temp:
            package.extract_tar(archive, Path(temp))
            header = (
                Path(temp) / "lancedb-c-abc123-linux-amd64" / "include" / "lancedb.h"
            ).read_bytes()
        self.assertEqual(
            header,
            package.render_header(
                (self.repo / "include" / "lancedb.h").read_bytes(), manifest["version"]
            ),
        )
        self.assertIn(
            '#define LANCEDB_VERSION_STRING "0.1.0-preview.1"\n'.encode(), header
        )
        self.assertEqual(
            (self.repo / "include" / "lancedb.h").read_bytes(),
            (package.HEADER_VERSION_DEFAULT + "\n").encode(),
        )

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
                    "lib/liblancedb.so.0.1",
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


class PackageChecksumTests(PackageFixture):
    def create_for_target(self, target_name: str, output_name: str):
        target = package.target_for(target_name)
        library_dir = self.root / f"library-{target.name}"
        library_dir.mkdir()
        (library_dir / target.library).write_bytes(
            f"fake {target.name} library".encode()
        )
        if target.import_library:
            (library_dir / target.import_library).write_text(
                "import library\n", encoding="utf-8"
            )
        output = self.root / output_name
        args = argparse.Namespace(
            target=target_name,
            commit="abc123",
            repo_root=str(self.repo),
            library_dir=str(library_dir),
            output_dir=str(output),
        )
        with contextlib.redirect_stdout(io.StringIO()):
            with mock.patch.dict(os.environ, {"LANCEDB_VERSION": "0.1.0-preview.1"}):
                package.create_package(args)
        archive = output / f"lancedb-c-abc123-{target.name}.tar.gz"
        return target, archive, output

    @unittest.skipUnless(shutil.which("sha256sum"), "GNU sha256sum is required")
    def test_windows_packages_have_canonical_checksums(self) -> None:
        targets = ("windows-amd64", "windows-arm64")
        for target_name in targets:
            with self.subTest(target=target_name):
                target, archive, output = self.create_for_target(
                    target_name, f"package-{target_name}"
                )
                checksum = output / f"{archive.name}.sha256"
                checksum_bytes = checksum.read_bytes()
                self.assertNotIn(b"\r", checksum_bytes)
                self.assertTrue(checksum_bytes.endswith(b"\n"))
                self.assertEqual(checksum_bytes.count(b"\n"), 1)
                subprocess.run(
                    ["sha256sum", "-c", checksum.name],
                    cwd=output,
                    check=True,
                    capture_output=True,
                    text=True,
                )

                with tempfile.TemporaryDirectory(
                    prefix=f"lancedb-c-{target.name}-"
                ) as temp:
                    root = Path(temp)
                    package.extract_tar(archive, root)
                    package_root = root / f"lancedb-c-abc123-{target.name}"
                    internal = (package_root / "SHA256SUMS").read_bytes()
                    self.assertNotIn(b"\r", internal)
                    self.assertTrue(internal.endswith(b"\n"))
                    subprocess.run(
                        ["sha256sum", "-c", "SHA256SUMS"],
                        cwd=package_root,
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(
                        (package_root / "LICENSE").read_bytes(),
                        (self.repo / "LICENSE").read_bytes(),
                    )
                    self.assertEqual(
                        (package_root / "include" / "lancedb.h").read_bytes(),
                        package.render_header(
                            (self.repo / "include" / "lancedb.h").read_bytes(),
                            "0.1.0-preview.1",
                        ),
                    )


class TargetPolicyTests(unittest.TestCase):
    def workflow(self) -> str:
        return (
            Path(__file__).resolve().parents[2] / ".github" / "workflows" / "packages.yml"
        ).read_text(encoding="utf-8")

    def test_workflow_matrix_matches_package_targets(self) -> None:
        matrix_targets = set(
            re.findall(r"^\s*-\s+target:\s+(\S+)\s*$", self.workflow(), re.MULTILINE)
        )
        self.assertEqual(matrix_targets, set(package.TARGETS))

    def test_package_workflow_runs_for_tags_and_dispatch_only(self) -> None:
        workflow = self.workflow()
        self.assertNotIn("pull_request:", workflow)
        self.assertIn("push:\n    tags:\n      - 'v*'", workflow)
        self.assertIn("workflow_dispatch:", workflow)

    def test_intel_macos_is_not_a_supported_target(self) -> None:
        with self.assertRaises(SystemExit):
            package.target_for("x86_64-apple-darwin")
        with self.assertRaises(SystemExit):
            package.target_for("darwin-amd64")
        self.assertNotIn("darwin-amd64", {target.name for target in package.TARGETS.values()})


class ConsumerEnvironmentTests(unittest.TestCase):
    def test_macos_does_not_inject_dyld_library_path(self) -> None:
        with mock.patch.dict(os.environ, {"DYLD_LIBRARY_PATH": "/inherited"}):
            env = package.consumer_environment(
                package.target_for("darwin-arm64"), Path("/tmp/package")
            )
        self.assertNotIn("DYLD_LIBRARY_PATH", env)

    def test_linux_prepends_the_package_library_path(self) -> None:
        with mock.patch.dict(os.environ, {"LD_LIBRARY_PATH": "/inherited"}):
            env = package.consumer_environment(
                package.target_for("x86_64-unknown-linux-gnu"), Path("/tmp/package")
            )
        self.assertEqual(
            env["LD_LIBRARY_PATH"],
            f"/tmp/package/lib{os.pathsep}/inherited",
        )


class ParserTests(unittest.TestCase):
    def test_parse_elf_soname(self) -> None:
        output = """\
 0x000000000000000e (SONAME)             Library soname: [liblancedb.so.0.1]
"""
        self.assertEqual(package.parse_elf_soname(output), "liblancedb.so.0.1")
        self.assertIsNone(package.parse_elf_soname("no soname here\n"))

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

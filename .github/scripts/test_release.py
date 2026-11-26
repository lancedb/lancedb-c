#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Local fixture checks for tagged release preparation and publication."""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import package
import release


VERSION = "0.1.0-preview.1"
TAG = f"v{VERSION}"
TAG_OBJECT = "a" * 40
COMMIT = "b" * 40
LANCEDB_REVISION = "85dcf3a3c5a5293471783e989cd189e144f8e3c1"
LANCE_REVISION = "a93690c0a82ad8ea5e0dd621e3a311a5fce9a487"


def cargo_lock() -> str:
    def block(name: str, version: str, source: str) -> str:
        return (
            "[[package]]\n"
            f'name = "{name}"\n'
            f'version = "{version}"\n'
            f'source = "{source}"\n'
            'dependencies = [\n "arrow",\n]\n\n'
        )

    return (
        block(
            "lancedb",
            "0.42.0-beta.36",
            "git+https://github.com/lancedb/lancedb?tag=v0.42.0-beta.36#"
            + LANCEDB_REVISION,
        )
        + block(
            "lance",
            "16.0.0-beta.3",
            "git+https://github.com/lance-format/lance.git?tag=v16.0.0-beta.3#"
            + LANCE_REVISION,
        )
        + block(
            "lance-io",
            "16.0.0-beta.3",
            "git+https://github.com/lance-format/lance.git?tag=v16.0.0-beta.3#"
            + LANCE_REVISION,
        )
        + block(
            "lance-namespace",
            "16.0.0-beta.3",
            "git+https://github.com/lance-format/lance.git?tag=v16.0.0-beta.3#"
            + LANCE_REVISION,
        )
        + block("arrow", "59.2.0", "registry+https://github.com/rust-lang/crates.io-index")
    )


def git(repo: Path, *args: str, check: bool = True) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        text=True,
        capture_output=True,
        check=False,
    )
    if check and result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout.strip()


class ReleaseFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="lancedb-c-release-test-")
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        (self.repo / "include").mkdir(parents=True)
        (self.repo / "LICENSE").write_text("license\n", encoding="utf-8")
        (self.repo / "include" / "lancedb.h").write_text(
            package.HEADER_VERSION_DEFAULT + "\n", encoding="utf-8"
        )
        (self.repo / "Cargo.lock").write_text(cargo_lock(), encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        git(self.repo, "config", "user.email", "test@example.com")
        git(self.repo, "config", "user.name", "Test")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "fixture")
        self.commit = git(self.repo, "rev-parse", "HEAD")
        git(self.repo, "tag", "-a", TAG, "-m", "release")
        self.tag_object = git(self.repo, "rev-parse", f"refs/tags/{TAG}")
        self.tag_commit = git(self.repo, "rev-parse", f"refs/tags/{TAG}^{{}}")
        self.assertEqual(self.tag_commit, self.commit)

        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.libraries = self.root / "libraries"
        self.libraries.mkdir()
        self.add_packages(self.artifacts, self.commit, VERSION)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def add_packages(self, output: Path, commit: str, version: str) -> None:
        for target_name in release.RELEASE_TARGETS:
            target = package.target_for(target_name)
            library_dir = self.libraries / f"{target.name}-{commit}"
            library_dir.mkdir(parents=True, exist_ok=True)
            (library_dir / target.library).write_bytes(
                f"fake {target.name} library".encode()
            )
            if target.import_library:
                (library_dir / target.import_library).write_text(
                    "import library\n", encoding="utf-8"
                )
            args = argparse.Namespace(
                target=target_name,
                commit=commit,
                repo_root=str(self.repo),
                library_dir=str(library_dir),
                output_dir=str(output),
            )
            with contextlib.redirect_stdout(io.StringIO()):
                with mock.patch.dict(os.environ, {"LANCEDB_VERSION": version}):
                    package.create_package(args)

    def prepare(
        self,
        output_name: str = "release",
        tag_object: str | None = None,
    ) -> Path:
        output = self.root / output_name
        args = argparse.Namespace(
            tag=TAG,
            tag_object=tag_object or self.tag_object,
            tag_commit=self.commit,
            head_commit=self.commit,
            repo_root=str(self.repo),
            artifacts=str(self.artifacts),
            output=str(output),
            github_output=None,
        )
        with contextlib.redirect_stdout(io.StringIO()):
            release.prepare(args)
        return output

    def metadata_path(self, output: Path) -> Path:
        return output / "release-metadata.json"


class TagTests(unittest.TestCase):
    def test_accepts_annotated_preview_tag(self) -> None:
        tag = release.parse_release_tag(TAG, TAG_OBJECT, COMMIT, COMMIT)
        self.assertEqual(tag.version, VERSION)
        self.assertTrue(tag.prerelease)
        self.assertEqual(tag.tag_object, TAG_OBJECT)
        self.assertEqual(tag.tag_type, "annotated")

    def test_accepts_lightweight_tag_with_matching_refs(self) -> None:
        tag = release.parse_release_tag(TAG, COMMIT, COMMIT, COMMIT)
        self.assertEqual(tag.tag_object, COMMIT)
        self.assertEqual(tag.commit, COMMIT)
        self.assertEqual(tag.tag_type, "lightweight")

    def test_rejects_tag_on_another_commit(self) -> None:
        with self.assertRaisesRegex(SystemExit, "peels to"):
            release.parse_release_tag(TAG, TAG_OBJECT, COMMIT, "c" * 40)

    def test_rejects_malformed_tag_and_commit(self) -> None:
        with self.assertRaisesRegex(SystemExit, "v<strict SemVer>"):
            release.parse_release_tag("preview", TAG_OBJECT, COMMIT, COMMIT)
        with self.assertRaisesRegex(SystemExit, "40-character"):
            release.parse_release_tag(TAG, "not-a-sha", COMMIT, COMMIT)

    def test_rejects_stable_tag(self) -> None:
        with self.assertRaisesRegex(SystemExit, "publishes preview tags only"):
            release.parse_release_tag("v1.0.0", TAG_OBJECT, COMMIT, COMMIT)


class PrepareTests(ReleaseFixture):
    def test_prepares_five_archives_checksums_and_notes(self) -> None:
        output = self.prepare()

        archives = sorted(output.glob("*.tar.gz"))
        checksums = sorted(output.glob("*.tar.gz.sha256"))
        self.assertEqual(len(archives), 5)
        self.assertEqual(len(checksums), 5)

        sums = (output / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(sums), 5)
        for line in sums:
            digest, name = line.split("  ", 1)
            self.assertEqual(len(digest), 64)
            self.assertTrue((output / name).is_file())
            self.assertEqual(digest, package.sha256_file(output / name))

        metadata = json.loads((output / "release-metadata.json").read_text())
        self.assertEqual(metadata["tag"], TAG)
        self.assertEqual(metadata["tagObject"], self.tag_object)
        self.assertEqual(metadata["tagType"], "annotated")
        self.assertEqual(metadata["commit"], self.commit)
        self.assertTrue(metadata["prerelease"])
        self.assertEqual(len(metadata["packages"]), 5)
        self.assertEqual(
            {entry["target"] for entry in metadata["packages"]},
            set(release.RELEASE_TARGETS),
        )

        notes = (output / "release-notes.md").read_text(encoding="utf-8")
        self.assertIn(TAG, notes)
        self.assertIn(self.tag_object, notes)
        self.assertIn("annotated", notes)
        self.assertIn(self.commit, notes)
        self.assertIn(LANCEDB_REVISION, notes)
        self.assertIn(LANCE_REVISION, notes)
        self.assertIn("preview release", notes)
        self.assertIn("only functions declared in `include/lancedb.h`", notes)
        self.assertIn("AWS-LC jitterentropy", notes)
        for entry in metadata["packages"]:
            self.assertIn(entry["archive"], notes)
            self.assertIn(entry["sha256"], notes)

    def test_records_lightweight_tag_and_peeled_refs(self) -> None:
        output = self.prepare("lightweight", tag_object=self.commit)
        metadata = json.loads((output / "release-metadata.json").read_text())
        self.assertEqual(metadata["tagObject"], self.commit)
        self.assertEqual(metadata["tagType"], "lightweight")
        notes = (output / "release-notes.md").read_text(encoding="utf-8")
        self.assertIn("| Tag ref object | `" + self.commit + "` |", notes)
        self.assertIn("| Peeled commit | `" + self.commit + "` |", notes)
        self.assertIn("lightweight", notes)

    @unittest.skipUnless(shutil.which("sha256sum"), "GNU sha256sum is required")
    def test_prepare_canonicalizes_crlf_inputs_and_checks_all_sums(self) -> None:
        windows_targets = ("windows-amd64", "windows-arm64")
        for target_name in windows_targets:
            target = package.target_for(target_name)
            checksum = (
                self.artifacts
                / f"lancedb-c-{self.commit}-{target.name}.tar.gz.sha256"
            )
            checksum.write_bytes(checksum.read_bytes().replace(b"\n", b"\r\n"))

        output = self.prepare()
        for target_name in windows_targets:
            target = package.target_for(target_name)
            archive = output / f"lancedb-c-{self.commit}-{target.name}.tar.gz"
            checksum = output / f"{archive.name}.sha256"
            checksum_bytes = checksum.read_bytes()
            self.assertNotIn(b"\r", checksum_bytes)
            self.assertEqual(checksum_bytes.count(b"\n"), 1)
            subprocess.run(
                ["sha256sum", "-c", checksum.name],
                cwd=output,
                check=True,
                capture_output=True,
                text=True,
            )
            with tempfile.TemporaryDirectory(
                prefix=f"lancedb-c-release-{target.name}-"
            ) as temp:
                root = Path(temp)
                package.extract_tar(archive, root)
                package_root = root / f"lancedb-c-{self.commit}-{target.name}"
                internal = (package_root / "SHA256SUMS").read_bytes()
                self.assertNotIn(b"\r", internal)
                subprocess.run(
                    ["sha256sum", "-c", "SHA256SUMS"],
                    cwd=package_root,
                    check=True,
                    capture_output=True,
                    text=True,
                )

        combined = (output / "SHA256SUMS").read_bytes()
        self.assertNotIn(b"\r", combined)
        subprocess.run(
            ["sha256sum", "-c", "SHA256SUMS"],
            cwd=output,
            check=True,
            capture_output=True,
            text=True,
        )

    def test_reads_the_public_header_from_the_tagged_commit(self) -> None:
        (self.repo / "include" / "lancedb.h").write_text(
            "dirty working tree\n", encoding="utf-8"
        )
        output = self.prepare()
        self.assertTrue((output / "release-metadata.json").is_file())

    def test_rejects_package_header_not_rendered_for_tag(self) -> None:
        target = package.target_for(release.RELEASE_TARGETS[0])
        archive = self.artifacts / f"lancedb-c-{self.commit}-{target.name}.tar.gz"
        with tempfile.TemporaryDirectory(prefix="lancedb-c-header-") as temp:
            root = Path(temp)
            package.extract_tar(archive, root)
            package_root = root / f"lancedb-c-{self.commit}-{target.name}"
            header = package_root / "include" / "lancedb.h"
            header.write_bytes(
                header.read_bytes().replace(
                    b'"0.1.0-preview.1"', b'"9.9.9"'
                )
            )
            sums_path = package_root / "SHA256SUMS"
            lines = sums_path.read_text(encoding="utf-8").splitlines()
            sums_path.write_text(
                "\n".join(
                    f"{package.sha256_file(header)}  include/lancedb.h"
                    if line.endswith("  include/lancedb.h")
                    else line
                    for line in lines
                )
                + "\n",
                encoding="utf-8",
            )
            with tarfile.open(archive, "w:gz") as tar:
                tar.add(package_root, arcname=package_root.name)
        digest = package.sha256_file(archive)
        (self.artifacts / f"{archive.name}.sha256").write_text(
            f"{digest}  {archive.name}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(SystemExit, "rendered header"):
            self.prepare()

    def test_rejects_missing_target(self) -> None:
        target = package.target_for(release.RELEASE_TARGETS[0])
        archive = self.artifacts / f"lancedb-c-{self.commit}-{target.name}.tar.gz"
        archive.unlink()
        with self.assertRaisesRegex(SystemExit, "missing release assets"):
            self.prepare()

    def test_rejects_extra_asset(self) -> None:
        (self.artifacts / "unexpected.tar.gz").write_bytes(b"unexpected")
        with self.assertRaisesRegex(SystemExit, "unexpected release assets"):
            self.prepare()

    def test_rejects_bad_archive_checksum(self) -> None:
        target = package.target_for(release.RELEASE_TARGETS[0])
        checksum = (
            self.artifacts
            / f"lancedb-c-{self.commit}-{target.name}.tar.gz.sha256"
        )
        checksum.write_text(f"{'0' * 64}  {checksum.name.removesuffix('.sha256')}\n")
        with self.assertRaisesRegex(SystemExit, "checksum mismatch"):
            self.prepare()

    def test_rejects_manifest_with_wrong_commit(self) -> None:
        target = package.target_for(release.RELEASE_TARGETS[0])
        wrong_commit = "c" * 40
        wrong = self.root / "wrong"
        wrong.mkdir()
        self.add_packages(wrong, wrong_commit, VERSION)
        wrong_archive = wrong / f"lancedb-c-{wrong_commit}-{target.name}.tar.gz"
        expected_archive = (
            self.artifacts / f"lancedb-c-{self.commit}-{target.name}.tar.gz"
        )
        with tempfile.TemporaryDirectory(prefix="lancedb-c-wrong-manifest-") as temp:
            root = Path(temp)
            package.extract_tar(wrong_archive, root)
            wrong_root = root / f"lancedb-c-{wrong_commit}-{target.name}"
            expected_root = root / f"lancedb-c-{self.commit}-{target.name}"
            wrong_root.rename(expected_root)
            with tarfile.open(expected_archive, "w:gz") as tar:
                tar.add(expected_root, arcname=expected_root.name)
        digest = package.sha256_file(expected_archive)
        (self.artifacts / f"{expected_archive.name}.sha256").write_text(
            f"{digest}  {expected_archive.name}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(SystemExit, "manifest mismatch"):
            self.prepare()

    def test_rejects_unlisted_package_member(self) -> None:
        target = package.target_for(release.RELEASE_TARGETS[0])
        archive = (
            self.artifacts / f"lancedb-c-{self.commit}-{target.name}.tar.gz"
        )
        with tempfile.TemporaryDirectory(prefix="lancedb-c-member-") as temp:
            root = Path(temp)
            package.extract_tar(archive, root)
            package_root = root / f"lancedb-c-{self.commit}-{target.name}"
            (package_root / "unexpected.txt").write_text("unexpected\n")
            with tarfile.open(archive, "w:gz") as tar:
                tar.add(package_root, arcname=package_root.name)
        digest = package.sha256_file(archive)
        (self.artifacts / f"{archive.name}.sha256").write_text(
            f"{digest}  {archive.name}\n", encoding="utf-8"
        )
        with self.assertRaisesRegex(SystemExit, "SHA256SUMS is missing"):
            self.prepare()


class RemoteTagTests(ReleaseFixture):
    def metadata(self) -> dict:
        return {
            "tag": TAG,
            "tagObject": self.tag_object,
            "tagType": "annotated",
            "commit": self.commit,
        }

    def test_accepts_matching_annotated_tag(self) -> None:
        release.verify_remote_tag(self.metadata(), str(self.repo))

    def test_accepts_matching_lightweight_tag(self) -> None:
        git(self.repo, "tag", "-f", TAG, self.commit)
        metadata = self.metadata()
        metadata["tagObject"] = self.commit
        metadata["tagType"] = "lightweight"
        release.verify_remote_tag(metadata, str(self.repo))

    def test_rejects_moved_tag_ref(self) -> None:
        git(self.repo, "tag", "-f", "-a", TAG, "-m", "moved", self.commit)
        with self.assertRaisesRegex(SystemExit, "ref object moved"):
            release.verify_remote_tag(self.metadata(), str(self.repo))

    def test_rejects_moved_peeled_commit(self) -> None:
        (self.repo / "moved").write_text("moved\n", encoding="utf-8")
        git(self.repo, "add", "moved")
        git(self.repo, "commit", "-qm", "moved")
        git(self.repo, "tag", "-f", "-a", TAG, "-m", "moved")
        with self.assertRaisesRegex(SystemExit, "peeled commit moved|ref object moved"):
            release.verify_remote_tag(self.metadata(), str(self.repo))

    def test_rejects_disappeared_tag(self) -> None:
        git(self.repo, "tag", "-d", TAG)
        with self.assertRaisesRegex(SystemExit, "disappeared"):
            release.verify_remote_tag(self.metadata(), str(self.repo))


class PublishTests(ReleaseFixture):
    def setUp(self) -> None:
        super().setUp()
        self.output = self.prepare()
        self.metadata = json.loads(
            self.metadata_path(self.output).read_text(encoding="utf-8")
        )
        self.assets = release.verify_output(self.metadata, self.metadata_path(self.output))
        self.digests = {asset.name: asset.sha256 for asset in self.assets}

    def asset_json(self, name: str, digest: str | None = None) -> dict:
        return {
            "name": name,
            "state": "uploaded",
            "digest": "sha256:" + (digest or self.digests[name]),
        }

    def release_json(self, assets: list | None = None, draft: bool = True) -> dict:
        return {
            "databaseId": 123,
            "tagName": TAG,
            "targetCommitish": TAG,
            "isDraft": draft,
            "isPrerelease": True,
            "assets": assets or [],
            "url": "https://example.test/release",
        }

    def fake_gh(self, state: dict):
        def call(args, check=True):
            command = " ".join(args)
            if args[:2] == ["release", "view"]:
                if isinstance(state.get("views"), list):
                    release_json = state["views"].pop(0)
                else:
                    release_json = state.get("release")
                if release_json is None:
                    return subprocess.CompletedProcess(
                        [command], 1, "", "release not found"
                    )
                state["release"] = release_json
                return subprocess.CompletedProcess(
                    [command], 0, json.dumps(release_json), ""
                )
            if args[:2] == ["release", "create"]:
                state["created"] = True
                state["release"] = self.release_json()
            elif args[:2] == ["release", "edit"]:
                state["edits"].append(args)
                if "--draft=false" in args:
                    state["release"]["isDraft"] = False
            elif args[:2] == ["release", "upload"]:
                state["uploads"].append(args)
                for path in args[5:]:
                    state["release"]["assets"].append(
                        self.asset_json(Path(path).name)
                    )
            else:
                raise AssertionError(f"unexpected gh command: {command}")
            return subprocess.CompletedProcess([command], 0, "", "")

        return call

    def publish(
        self,
        state: dict,
        remote: str | None = None,
        side_effect=None,
    ) -> None:
        arguments = argparse.Namespace(
            metadata=str(self.metadata_path(self.output)),
            repository="lancedb/lancedb-c",
            remote=remote or str(self.repo),
        )
        with mock.patch.object(
            release, "run_gh", side_effect=side_effect or self.fake_gh(state)
        ):
            with contextlib.redirect_stdout(io.StringIO()):
                release.publish(arguments)

    def test_publishes_draft_only_after_all_assets_verify(self) -> None:
        state = {"release": None, "created": False, "edits": [], "uploads": []}
        self.publish(state)

        self.assertTrue(state["created"])
        self.assertFalse(state["release"]["isDraft"])
        self.assertEqual(
            {asset["name"] for asset in state["release"]["assets"]},
            {asset.name for asset in self.assets},
        )
        self.assertEqual(len(state["uploads"]), 1)
        self.assertNotIn("--clobber", state["uploads"][0])
        self.assertEqual(len(state["uploads"][0]) - 5, len(self.assets))

    def test_reuses_an_existing_draft_with_expected_assets(self) -> None:
        state = {
            "release": self.release_json(
                [self.asset_json(asset.name) for asset in self.assets]
            ),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        self.publish(state)

        self.assertFalse(state["created"])
        self.assertFalse(state["release"]["isDraft"])
        self.assertEqual(state["uploads"], [])

    def test_partial_retry_uploads_only_missing_assets(self) -> None:
        existing = self.asset_json(self.assets[0].name)
        state = {
            "release": self.release_json([existing]),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        self.publish(state)

        self.assertEqual(len(state["uploads"]), 1)
        uploaded = {Path(path).name for path in state["uploads"][0][5:]}
        self.assertEqual(
            uploaded, {asset.name for asset in self.assets[1:]}
        )
        self.assertEqual(
            {asset["name"] for asset in state["release"]["assets"]},
            {asset.name for asset in self.assets},
        )

    def test_starter_asset_fails_closed_then_retry_succeeds(self) -> None:
        starter = self.asset_json(self.assets[0].name)
        starter.update({"state": "starter", "digest": None, "size": 0})
        state = {
            "release": self.release_json([starter]),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        with self.assertRaisesRegex(SystemExit, "empty starter"):
            self.publish(state)
        self.assertEqual(state["edits"], [])
        self.assertEqual(state["uploads"], [])

        # This is the documented operator action: remove only the empty
        # placeholder from the exact draft, leaving every other asset in place.
        state["release"]["assets"] = []
        state["edits"].clear()
        state["uploads"].clear()
        self.publish(state)
        self.assertFalse(state["release"]["isDraft"])
        self.assertEqual(
            {asset["name"] for asset in state["release"]["assets"]},
            {asset.name for asset in self.assets},
        )

    def test_rejects_draft_asset_digest_mismatch_before_writes(self) -> None:
        assets = [self.asset_json(asset.name) for asset in self.assets]
        assets[0]["digest"] = "sha256:" + ("0" * 64)
        state = {
            "release": self.release_json(assets),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        with self.assertRaisesRegex(SystemExit, "digest mismatch"):
            self.publish(state)
        self.assertEqual(state["edits"], [])
        self.assertEqual(state["uploads"], [])

    def test_rejects_draft_that_became_public_before_retry(self) -> None:
        draft = self.release_json(
            [self.asset_json(asset.name) for asset in self.assets]
        )
        public = dict(draft)
        public["isDraft"] = False
        state = {
            "views": [draft, public],
            "created": False,
            "edits": [],
            "uploads": [],
        }
        with self.assertRaisesRegex(SystemExit, "became public"):
            self.publish(state)
        self.assertEqual(state["edits"], [])
        self.assertEqual(state["uploads"], [])

    def test_rejects_tag_moved_after_draft_check(self) -> None:
        state = {
            "release": self.release_json(
                [self.asset_json(asset.name) for asset in self.assets]
            ),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        base = self.fake_gh(state)
        moved = {"done": False}

        def call(args, check=True):
            result = base(args, check)
            if (
                args[:2] == ["release", "edit"]
                and "--draft=false" not in args
                and not moved["done"]
            ):
                git(self.repo, "tag", "-f", "-a", TAG, "-m", "moved", self.commit)
                moved["done"] = True
            return result

        with self.assertRaisesRegex(SystemExit, "ref object moved"):
            self.publish(state, side_effect=call)
        self.assertTrue(state["release"]["isDraft"])
        self.assertFalse(
            any("--draft=false" in args for args in state["edits"])
        )

    def test_rejects_moved_tag_before_draft_creation(self) -> None:
        state = {"release": None, "created": False, "edits": [], "uploads": []}
        git(self.repo, "tag", "-f", "-a", TAG, "-m", "moved", self.commit)
        with self.assertRaisesRegex(SystemExit, "ref object moved"):
            self.publish(state)
        self.assertFalse(state["created"])

    def test_rejects_existing_published_release(self) -> None:
        state = {
            "release": self.release_json(draft=False),
            "created": False,
            "edits": [],
            "uploads": [],
        }
        with self.assertRaisesRegex(SystemExit, "already published"):
            self.publish(state)
        self.assertEqual(state["edits"], [])
        self.assertEqual(state["uploads"], [])

    def test_rejects_stable_metadata_even_with_true_prerelease_flag(self) -> None:
        self.metadata["tag"] = "v1.0.0"
        self.metadata["version"] = "1.0.0"
        self.metadata["title"] = "lancedb-c v1.0.0"
        self.metadata["prerelease"] = True
        self.metadata_path(self.output).write_text(
            json.dumps(self.metadata), encoding="utf-8"
        )
        state = {"release": None, "created": False, "edits": [], "uploads": []}
        with self.assertRaisesRegex(SystemExit, "prerelease flag disagrees"):
            self.publish(state)
        self.assertFalse(state["created"])

    def test_rejects_wrong_repository(self) -> None:
        arguments = argparse.Namespace(
            metadata=str(self.metadata_path(self.output)),
            repository="lancedb/not-lancedb-c",
            remote=str(self.repo),
        )
        with self.assertRaisesRegex(SystemExit, "expected 'lancedb/lancedb-c'"):
            release.publish(arguments)


class LineEndingTests(unittest.TestCase):
    def test_narrow_attributes_keep_package_sources_lf(self) -> None:
        repo_root = Path(__file__).resolve().parents[2]
        attributes = (repo_root / ".gitattributes").read_text(encoding="utf-8")
        self.assertIn("LICENSE text eol=lf", attributes)
        self.assertIn("include/lancedb.h text eol=lf", attributes)
        for relative in ("LICENSE", "include/lancedb.h"):
            self.assertNotIn(b"\r\n", (repo_root / relative).read_bytes())

        with tempfile.TemporaryDirectory(prefix="lancedb-c-line-endings-") as temp:
            root = Path(temp)
            source = root / "source"
            (source / "include").mkdir(parents=True)
            shutil.copyfile(repo_root / "LICENSE", source / "LICENSE")
            shutil.copyfile(
                repo_root / "include" / "lancedb.h", source / "include" / "lancedb.h"
            )
            shutil.copyfile(repo_root / ".gitattributes", source / ".gitattributes")
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            git(source, "config", "user.email", "test@example.com")
            git(source, "config", "user.name", "Test")
            git(source, "add", ".")
            git(source, "commit", "-qm", "fixture")

            expected = {
                relative: subprocess.check_output(
                    ["git", "-C", str(source), "show", f"HEAD:{relative}"]
                )
                for relative in ("LICENSE", "include/lancedb.h")
            }
            library = root / "library"
            library.mkdir()
            (library / "liblancedb.so").write_bytes(b"fake library")
            archives = []
            for autocrlf in ("true", "false"):
                checkout = root / f"checkout-{autocrlf}"
                subprocess.run(
                    ["git", "clone", "-q", str(source), str(checkout)], check=True
                )
                git(checkout, "config", "core.autocrlf", autocrlf)
                git(checkout, "checkout", "-f", "HEAD", "--", "LICENSE", "include/lancedb.h")
                for relative, blob in expected.items():
                    self.assertEqual((checkout / relative).read_bytes(), blob)
                    self.assertNotIn(b"\r\n", (checkout / relative).read_bytes())
                output = root / f"package-{autocrlf}"
                arguments = argparse.Namespace(
                    target="x86_64-unknown-linux-gnu",
                    commit="abc123",
                    repo_root=str(checkout),
                    library_dir=str(library),
                    output_dir=str(output),
                )
                with contextlib.redirect_stdout(io.StringIO()):
                    with mock.patch.dict(
                        os.environ, {"LANCEDB_VERSION": VERSION}
                    ):
                        package.create_package(arguments)
                archives.append(
                    output / "lancedb-c-abc123-linux-amd64.tar.gz"
                )
            self.assertEqual(archives[0].read_bytes(), archives[1].read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)

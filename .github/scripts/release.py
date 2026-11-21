#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Verify tagged lancedb-c packages and publish their GitHub Release.

The package matrix uploads one archive and one sibling checksum per supported
target.  This module treats those artifacts as untrusted input: it re-verifies
their contents against the exact tag, writes the combined checksum inventory
and release notes, and only then exposes a draft release.

The release notes record the tag ref object, peeled commit, the locked LanceDB
and Lance revisions, and the SHA-256 of every published archive. Publication is
limited to preview tags; stable releases remain under Xuanwo's ownership.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
from pathlib import Path
from typing import Dict, List, Optional

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import package
import version as version_policy


STABLE_OWNER = "Xuanwo"
EXPECTED_REPOSITORY = "lancedb/lancedb-c"
RELEASE_TARGETS = (
    "linux-amd64",
    "linux-arm64",
    "darwin-arm64",
    "windows-amd64",
    "windows-arm64",
)
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
PACKAGE_SECTION = re.compile(r"^\[\[package\]\]\s*$", re.MULTILINE)
PACKAGE_FIELD = re.compile(r"^(name|version|source) = (.+)$")


@dataclasses.dataclass(frozen=True)
class ReleaseTag:
    name: str
    version: str
    tag_object: str
    commit: str
    prerelease: bool
    tag_type: str


@dataclasses.dataclass(frozen=True)
class LockedPackage:
    name: str
    version: str
    revision: str
    tag: Optional[str]


@dataclasses.dataclass(frozen=True)
class PackageArtifact:
    target: package.Target
    archive: Path
    checksum: Path
    sha256: str


@dataclasses.dataclass(frozen=True)
class ReleaseAsset:
    name: str
    path: Path
    sha256: str


def fail(message: str) -> None:
    raise SystemExit(message)


def parse_release_tag(
    name: str,
    tag_object: str,
    commit: str,
    head_commit: str,
) -> ReleaseTag:
    try:
        release_version = version_policy.validate_tag(name)
    except ValueError as error:
        fail(str(error))
    for label, value in (
        ("tag ref object", tag_object),
        ("peeled commit", commit),
        ("checked-out head", head_commit),
    ):
        if not SHA_PATTERN.fullmatch(value):
            fail(f"{label} is not a 40-character commit id: {value!r}")
    if commit != head_commit:
        fail(f"{name} peels to {commit}, not the checked-out {head_commit}")

    prerelease = not version_policy.is_stable(release_version)
    if not prerelease:
        fail(
            f"{name} is a stable tag; this workflow publishes preview tags only, "
            f"while stable publication remains under {STABLE_OWNER}'s ownership"
        )
    tag_type = "lightweight" if tag_object == commit else "annotated"
    return ReleaseTag(name, release_version, tag_object, commit, prerelease, tag_type)


def verify_remote_tag(metadata: dict, remote: str) -> None:
    """Read the tag ref and peeled object from the exact remote without writing.

    GitHub exposes tag reads and release writes as separate API calls, so this
    check narrows, but cannot eliminate, a tag move between the check and a
    release write. Atomicity would require server-side tag immutability and
    release protection; this workflow does not change repository settings.
    """
    tag = metadata["tag"]
    ref = f"refs/tags/{tag}"
    peeled_ref = f"{ref}^{{}}"
    completed = subprocess.run(
        ["git", "ls-remote", remote, ref, peeled_ref],
        text=True,
        capture_output=True,
        env=os.environ.copy(),
    )
    if completed.returncode != 0:
        fail(
            f"git ls-remote failed for {remote} {ref}: "
            f"{completed.stderr.strip()}"
        )
    entries: Dict[str, str] = {}
    for line in completed.stdout.splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) != 2:
            fail(f"unexpected git ls-remote line: {line!r}")
        sha, name = fields
        if name in entries:
            fail(f"duplicate git ls-remote ref: {name}")
        entries[name] = sha
    if ref not in entries:
        fail(f"release tag disappeared from {remote}: {ref}")
    remote_object = entries[ref]
    if peeled_ref in entries:
        remote_commit = entries[peeled_ref]
        remote_type = "annotated"
    else:
        remote_commit = remote_object
        remote_type = "lightweight"
    if remote_object != metadata["tagObject"]:
        fail(
            f"release tag ref object moved: {remote_object} != "
            f"{metadata['tagObject']}"
        )
    if remote_commit != metadata["commit"]:
        fail(
            f"release tag peeled commit moved: {remote_commit} != "
            f"{metadata['commit']}"
        )
    if remote_type != metadata.get("tagType"):
        fail(
            f"release tag type changed: {remote_type} != "
            f"{metadata.get('tagType')}"
        )


def read_locked_packages(repo_root: Path) -> Dict[str, LockedPackage]:
    lock_file = repo_root / "Cargo.lock"
    if not lock_file.is_file():
        fail(f"missing {lock_file}")
    text = lock_file.read_text(encoding="utf-8")
    blocks = PACKAGE_SECTION.split(text)[1:]
    locked: Dict[str, LockedPackage] = {}
    for block in blocks:
        fields: Dict[str, str] = {}
        for line in block.splitlines():
            match = PACKAGE_FIELD.match(line)
            if match:
                try:
                    fields[match.group(1)] = json.loads(match.group(2))
                except json.JSONDecodeError:
                    fail(f"invalid Cargo.lock field: {line!r}")
        name = fields.get("name")
        if not name:
            continue
        if name in ("lancedb", "lance", "lance-io", "lance-namespace"):
            if name in locked:
                fail(f"Cargo.lock has more than one {name} package")
            locked[name] = locked_package(fields)
    missing = {"lancedb", "lance", "lance-io", "lance-namespace"} - locked.keys()
    if missing:
        fail(f"Cargo.lock is missing: {', '.join(sorted(missing))}")
    lance_revisions = {
        locked[name].revision
        for name in ("lance", "lance-io", "lance-namespace")
    }
    if len(lance_revisions) != 1:
        fail(f"Lance packages have different revisions: {sorted(lance_revisions)}")
    return locked


def locked_package(fields: Dict[str, str]) -> LockedPackage:
    name = fields["name"]
    version = fields.get("version")
    source = fields.get("source", "")
    if not version:
        fail(f"Cargo.lock package {name} has no version")
    if not source.startswith("git+"):
        fail(f"Cargo.lock package {name} is not pinned to a git source")
    parsed = urllib.parse.urlsplit(source[4:])
    revision = parsed.fragment
    if not SHA_PATTERN.fullmatch(revision):
        fail(f"Cargo.lock package {name} has no exact git revision")
    query = urllib.parse.parse_qs(parsed.query)
    tag = query.get("tag", [None])[0]
    return LockedPackage(name, version, revision, tag)


def release_targets() -> List[package.Target]:
    targets = []
    for name in RELEASE_TARGETS:
        try:
            targets.append(package.target_for(name))
        except SystemExit:
            fail(f"release target is not defined by the package script: {name}")
    return targets


def expected_names(commit: str) -> Dict[str, str]:
    names: Dict[str, str] = {}
    for target in release_targets():
        archive = f"{package.PACKAGE_NAME}-{commit}-{target.name}.tar.gz"
        checksum = f"{archive}.sha256"
        names[archive] = "archive"
        names[checksum] = "checksum"
    return names


def collect_artifacts(
    artifacts: Path, repo_root: Path, tag: ReleaseTag
) -> List[PackageArtifact]:
    if not artifacts.is_dir():
        fail(f"missing artifact directory: {artifacts}")
    files = [path for path in artifacts.rglob("*") if path.is_file()]
    by_name: Dict[str, Path] = {}
    for path in files:
        if path.name in by_name:
            fail(f"duplicate package artifact: {path.name}")
        by_name[path.name] = path

    expected = expected_names(tag.commit)
    missing = sorted(set(expected) - set(by_name))
    extra = sorted(set(by_name) - set(expected))
    if missing:
        fail(f"missing release assets: {', '.join(missing)}")
    if extra:
        fail(f"unexpected release assets: {', '.join(extra)}")

    result: List[PackageArtifact] = []
    for target in release_targets():
        archive_name = f"{package.PACKAGE_NAME}-{tag.commit}-{target.name}.tar.gz"
        archive = by_name[archive_name]
        checksum = by_name[f"{archive_name}.sha256"]
        digest = package.verify_archive_checksum(archive, checksum)
        verify_archive_contents(archive, repo_root, target, tag)
        result.append(PackageArtifact(target, archive, checksum, digest))
    return result


def verify_archive_contents(
    archive: Path, repo_root: Path, target: package.Target, tag: ReleaseTag
) -> None:
    package_root_name = f"{package.PACKAGE_NAME}-{tag.commit}-{target.name}"
    with tempfile.TemporaryDirectory(prefix="lancedb-c-release-verify-") as temp:
        work_dir = Path(temp)
        package.extract_tar(archive, work_dir)
        top_level = sorted(path.name for path in work_dir.iterdir())
        if top_level != [package_root_name]:
            fail(f"{archive.name} does not contain exactly one package root: {top_level}")
        package_root = package.find_package_root(work_dir, package_root_name)

        manifest_path = package_root / "manifest.json"
        if not manifest_path.is_file():
            fail(f"{archive.name} has no manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        expected_manifest = package.package_manifest(target, tag.commit, tag.version)
        if manifest != expected_manifest:
            fail(
                f"{archive.name} manifest mismatch:\n"
                f"{json.dumps(manifest, indent=2)}\n!=\n"
                f"{json.dumps(expected_manifest, indent=2)}"
            )

        package.verify_checksums(package_root)
        inventory = package.read_checksum_inventory(package_root)
        expected_files = {
            "LICENSE",
            manifest["include"],
            manifest["library"],
            "manifest.json",
        }
        if manifest["importLibrary"]:
            expected_files.add(manifest["importLibrary"])
        if set(inventory) != expected_files:
            fail(
                f"{archive.name} has unexpected package members: "
                f"{sorted(set(inventory) - expected_files)}"
            )
        actual_members = {
            path.relative_to(package_root).as_posix()
            for path in package_root.rglob("*")
        }
        expected_members = {
            "include",
            "include/lancedb.h",
            "lib",
            manifest["library"],
            manifest["license"],
            "manifest.json",
            "SHA256SUMS",
        }
        if manifest["importLibrary"]:
            expected_members.add(manifest["importLibrary"])
        if actual_members != expected_members:
            fail(
                f"{archive.name} has unexpected package paths: "
                f"{sorted(actual_members - expected_members)}"
            )

        for relative in (manifest["license"], manifest["include"]):
            if package.sha256_file(package_root / relative) != package.sha256_file(
                repo_root / relative
            ):
                fail(f"{archive.name} {relative} does not match the tagged source")


def render_notes(
    tag: ReleaseTag, packages: List[PackageArtifact], locked: Dict[str, LockedPackage]
) -> str:
    lancedb = locked["lancedb"]
    lance = locked["lance"]
    lines = [
        f"# lancedb-c {tag.name}",
        "",
        f"Prebuilt C shared libraries for {len(packages)} supported native targets.",
        "",
    ]
    if tag.prerelease:
        lines.extend(
            [
                "This is a preview release. The C ABI may change before 1.0; "
                "recompile C and C++ callers against the published header.",
                "",
            ]
        )
    lines.extend(
        [
            "## Release provenance",
            "",
            "| Field | Value |",
            "| --- | --- |",
            f"| Tag | `{tag.name}` |",
            f"| Tag ref object | `{tag.tag_object}` |",
            f"| Tag type | `{tag.tag_type}` |",
            f"| Peeled commit | `{tag.commit}` |",
            f"| Package version | `{tag.version}` |",
            f"| LanceDB SDK | `lancedb {lancedb.version}`"
            + (f" (`{lancedb.tag}`)" if lancedb.tag else "")
            + f", revision `{lancedb.revision}` |",
            f"| Lance | `{lance.version}`"
            + (f" (`{lance.tag}`)" if lance.tag else "")
            + f", revision `{lance.revision}` |",
            "",
            "The Lance revision covers `lance`, `lance-io` and `lance-namespace`.",
            "",
            "## Packages",
            "",
            "| Target | Archive | SHA-256 |",
            "| --- | --- | --- |",
        ]
    )
    for artifact in packages:
        lines.append(
            f"| `{artifact.target.name}` | `{artifact.archive.name}` "
            f"| `{artifact.sha256}` |"
        )
    lines.extend(
        [
            "",
            "Each archive has a sibling `.sha256` file. `SHA256SUMS` lists all "
            f"{len(packages)} archives.",
            "",
            "Archives contain `LICENSE`, `include/lancedb.h`, the target shared "
            "library, `manifest.json` and an internal `SHA256SUMS`. The manifest "
            "records the tag-derived version and peeled commit.",
            "",
        ]
    )
    return "\n".join(lines)


def set_github_output(path: Optional[str], values: Dict[str, str]) -> None:
    if not path:
        return
    with Path(path).open("a", encoding="utf-8") as file:
        for key, value in values.items():
            file.write(f"{key}={value}\n")


def prepare(args: argparse.Namespace) -> None:
    tag = parse_release_tag(
        args.tag, args.tag_object, args.tag_commit, args.head_commit
    )
    repo_root = Path(args.repo_root).resolve()
    locked = read_locked_packages(repo_root)
    packages = collect_artifacts(Path(args.artifacts).resolve(), repo_root, tag)

    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    for artifact in packages:
        shutil.copyfile(artifact.archive, output / artifact.archive.name)
        shutil.copyfile(artifact.checksum, output / artifact.checksum.name)

    sums = "\n".join(
        f"{artifact.sha256}  {artifact.archive.name}" for artifact in packages
    )
    (output / "SHA256SUMS").write_text(sums + "\n", encoding="utf-8")
    notes_name = "release-notes.md"
    (output / notes_name).write_text(
        render_notes(tag, packages, locked), encoding="utf-8"
    )

    metadata = {
        "tag": tag.name,
        "version": tag.version,
        "title": f"lancedb-c {tag.name}",
        "prerelease": tag.prerelease,
        "tagObject": tag.tag_object,
        "tagType": tag.tag_type,
        "commit": tag.commit,
        "notes": notes_name,
        "assets": [
            name
            for artifact in packages
            for name in (artifact.archive.name, artifact.checksum.name)
        ]
        + ["SHA256SUMS"],
        "packages": [
            {
                "target": artifact.target.name,
                "archive": artifact.archive.name,
                "sha256": artifact.sha256,
            }
            for artifact in packages
        ],
    }
    (output / "release-metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    set_github_output(
        args.github_output,
        {
            "title": metadata["title"],
            "prerelease": str(metadata["prerelease"]).lower(),
            "version": metadata["version"],
        },
    )
    print(json.dumps(metadata, indent=2))


def run_gh(args: List[str], check: bool = True) -> subprocess.CompletedProcess:
    command = ["gh", *args]
    completed = subprocess.run(
        command,
        text=True,
        capture_output=True,
        env=os.environ.copy(),
    )
    if check and completed.returncode != 0:
        fail(
            f"gh command failed with exit code {completed.returncode}: "
            f"{' '.join(command)}\n{completed.stderr.strip()}"
        )
    return completed


def view_release(tag: str, repository: str) -> Optional[dict]:
    completed = run_gh(
        [
            "release",
            "view",
            tag,
            "--repo",
            repository,
            "--json",
            "databaseId,isDraft,isPrerelease,assets,tagName,url",
        ],
        check=False,
    )
    if completed.returncode != 0:
        message = (completed.stdout + completed.stderr).lower()
        if "release not found" in message or "http 404" in message:
            return None
        fail(
            f"could not read release {tag}: {completed.stderr.strip()}"
        )
    return json.loads(completed.stdout)


def load_metadata(path: Path) -> dict:
    if not path.is_file():
        fail(f"missing release metadata: {path}")
    metadata = json.loads(path.read_text(encoding="utf-8"))
    for key in (
        "tag",
        "title",
        "prerelease",
        "tagObject",
        "tagType",
        "commit",
        "notes",
        "assets",
    ):
        if key not in metadata:
            fail(f"release metadata is missing {key}")
    if not isinstance(metadata["prerelease"], bool):
        fail("release metadata has a non-boolean prerelease flag")
    if metadata["tagType"] not in ("annotated", "lightweight"):
        fail("release metadata has an invalid tag type")
    if not isinstance(metadata["title"], str) or "\n" in metadata["title"]:
        fail("release metadata has an invalid title")
    try:
        expected_version = version_policy.validate_tag(metadata["tag"])
    except ValueError as error:
        fail(str(error))
    if metadata.get("version") != expected_version:
        fail("release metadata version does not match its tag")
    if metadata["prerelease"] != (not version_policy.is_stable(expected_version)):
        fail("release metadata prerelease flag disagrees with its tag")
    for label in ("tagObject", "commit"):
        if not isinstance(metadata[label], str) or not SHA_PATTERN.fullmatch(
            metadata[label]
        ):
            fail(f"release metadata has an invalid {label}")
    notes = metadata["notes"]
    if not isinstance(notes, str) or Path(notes).name != notes or not notes.endswith(
        ".md"
    ):
        fail("release metadata has an invalid notes path")
    assets = metadata["assets"]
    if not isinstance(assets, list) or not assets or len(set(assets)) != len(assets):
        fail("release metadata has invalid asset names")
    if not all(
        isinstance(name, str) and "/" not in name and ".." not in name
        for name in assets
    ):
        fail("release metadata has invalid asset names")

    packages = metadata.get("packages")
    if not isinstance(packages, list) or len(packages) != len(RELEASE_TARGETS):
        fail("release metadata has the wrong package count")
    expected_assets = set()
    seen_targets = set()
    for entry in packages:
        if not isinstance(entry, dict):
            fail("release metadata has an invalid package entry")
        target_name = entry.get("target")
        archive = entry.get("archive")
        digest = entry.get("sha256")
        if target_name not in RELEASE_TARGETS or target_name in seen_targets:
            fail(f"release metadata has an invalid target: {target_name!r}")
        if archive != f"{package.PACKAGE_NAME}-{metadata['commit']}-{target_name}.tar.gz":
            fail(f"release metadata has an invalid archive: {archive!r}")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail(f"release metadata has an invalid checksum for {target_name}")
        seen_targets.add(target_name)
        expected_assets.add(archive)
        expected_assets.add(f"{archive}.sha256")
    expected_assets.add("SHA256SUMS")
    if set(assets) != expected_assets:
        fail("release metadata assets do not match its packages")
    return metadata


def verify_output(metadata: dict, metadata_path: Path) -> List[ReleaseAsset]:
    output = metadata_path.parent
    expected = set(metadata["assets"])
    actual = {
        path.name
        for path in output.iterdir()
        if path.is_file()
        and (path.suffix in (".gz", ".sha256") or path.name == "SHA256SUMS")
    }
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        fail(f"prepared release assets differ; missing={missing}, extra={extra}")
    expected_sums = [
        f"{entry['sha256']}  {entry['archive']}" for entry in metadata["packages"]
    ]
    actual_sums = (output / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    if actual_sums != expected_sums:
        fail("prepared SHA256SUMS does not match the package metadata")

    digests: Dict[str, str] = {}
    for package_entry in metadata["packages"]:
        archive = output / package_entry["archive"]
        checksum = output / f"{package_entry['archive']}.sha256"
        digest = package.verify_archive_checksum(archive, checksum)
        if digest != package_entry["sha256"]:
            fail(f"prepared checksum mismatch for {archive.name}")
        digests[archive.name] = digest
        digests[checksum.name] = package.sha256_file(checksum)
    digests["SHA256SUMS"] = package.sha256_file(output / "SHA256SUMS")
    if set(digests) != expected:
        fail("prepared asset digests do not match the metadata")
    return [
        ReleaseAsset(name, output / name, digests[name]) for name in metadata["assets"]
    ]


def normalized_asset_digest(asset: dict) -> Optional[str]:
    digest = asset.get("digest")
    if not isinstance(digest, str):
        return None
    if digest.startswith("sha256:"):
        digest = digest.removeprefix("sha256:")
    return digest if re.fullmatch(r"[0-9a-f]{64}", digest) else None


def check_release_assets(
    release: dict, expected: Dict[str, str], require_all: bool
) -> set:
    present = set()
    for asset in release.get("assets", []):
        name = asset.get("name")
        if not isinstance(name, str):
            fail("release contains an asset without a name")
        if name in present:
            fail(f"release contains duplicate asset: {name}")
        if name not in expected:
            fail(f"release contains unexpected asset: {name}")
        if asset.get("state") != "uploaded":
            fail(f"release asset {name} is not in the uploaded state")
        digest = normalized_asset_digest(asset)
        if digest is None:
            fail(f"cannot verify the existing digest of release asset {name}")
        if digest != expected[name]:
            fail(f"release asset digest mismatch for {name}: {digest} != {expected[name]}")
        present.add(name)
    if require_all and present != set(expected):
        fail(
            "release is missing prepared assets: "
            + ", ".join(sorted(set(expected) - present))
        )
    return present


def check_release_identity(
    release: dict, metadata: dict, release_id: Optional[int]
) -> None:
    if release.get("tagName") != metadata["tag"]:
        fail(f"release tag name changed: {release.get('tagName')!r}")
    if release_id is not None and release.get("databaseId") != release_id:
        fail(
            "release identity changed: "
            f"{release.get('databaseId')!r} != {release_id!r}"
        )


def refresh_draft(
    metadata: dict,
    repository: str,
    expected: Dict[str, str],
    release_id: int,
    require_all: bool,
) -> tuple:
    release = view_release(metadata["tag"], repository)
    if release is None:
        fail(f"release {metadata['tag']} disappeared")
    check_release_identity(release, metadata, release_id)
    if not release.get("isDraft"):
        fail(f"release {metadata['tag']} became public")
    return release, check_release_assets(release, expected, require_all)


def ensure_release(
    metadata: dict, repository: str, remote: str, notes_file: Path
) -> dict:
    tag = metadata["tag"]
    existing = view_release(tag, repository)
    if existing is None:
        # Check the exact ref immediately before the first release write.
        verify_remote_tag(metadata, remote)
        command = [
            "release",
            "create",
            tag,
            "--repo",
            repository,
            "--verify-tag",
            "--draft",
            "--title",
            metadata["title"],
            "--notes-file",
            str(notes_file),
        ]
        if metadata["prerelease"]:
            command.append("--prerelease")
        run_gh(command)
        existing = view_release(tag, repository)
        if existing is None:
            fail(f"release {tag} was not created")
        check_release_identity(existing, metadata, None)
        if not existing.get("isDraft"):
            fail(f"release {tag} was created public")
        return existing

    check_release_identity(existing, metadata, None)
    if not existing.get("isDraft"):
        fail(f"release {tag} is already published")
    return existing


def publish(args: argparse.Namespace) -> None:
    metadata_path = Path(args.metadata).resolve()
    metadata = load_metadata(metadata_path)
    try:
        release_version = version_policy.validate_tag(metadata["tag"])
    except ValueError as error:
        fail(str(error))
    if version_policy.is_stable(release_version):
        fail(
            f"{metadata['tag']} is a stable tag; this workflow publishes preview "
            f"tags only, while stable publication remains under {STABLE_OWNER}'s ownership"
        )
    if metadata["prerelease"] != (not version_policy.is_stable(release_version)):
        fail("release metadata prerelease flag disagrees with its tag")

    repository = args.repository
    if repository != EXPECTED_REPOSITORY:
        fail(
            f"release repository is {repository!r}, expected {EXPECTED_REPOSITORY!r}"
        )
    remote = args.remote or f"https://github.com/{repository}.git"

    assets = verify_output(metadata, metadata_path)
    expected = {asset.name: asset.sha256 for asset in assets}
    notes_file = metadata_path.parent / metadata["notes"]
    release = ensure_release(metadata, repository, remote, notes_file)
    release_id = release.get("databaseId")
    if not isinstance(release_id, int):
        fail("release has no numeric database id")

    release, _ = refresh_draft(metadata, repository, expected, release_id, False)
    verify_remote_tag(metadata, remote)
    release, _ = refresh_draft(metadata, repository, expected, release_id, False)
    run_gh(
        [
            "release",
            "edit",
            metadata["tag"],
            "--repo",
            repository,
            "--title",
            metadata["title"],
            "--notes-file",
            str(notes_file),
            "--prerelease" if metadata["prerelease"] else "--prerelease=false",
        ]
    )
    release, present = refresh_draft(metadata, repository, expected, release_id, False)

    missing = [
        asset for asset in assets if asset.name not in present
    ]
    if missing:
        verify_remote_tag(metadata, remote)
        release, present = refresh_draft(metadata, repository, expected, release_id, False)
        run_gh(
            [
                "release",
                "upload",
                metadata["tag"],
                "--repo",
                repository,
                *[str(asset.path) for asset in missing],
            ]
        )
        release, present = refresh_draft(metadata, repository, expected, release_id, True)
    else:
        release, present = refresh_draft(metadata, repository, expected, release_id, True)

    verify_remote_tag(metadata, remote)
    release, present = refresh_draft(metadata, repository, expected, release_id, True)
    run_gh(
        [
            "release",
            "edit",
            metadata["tag"],
            "--repo",
            repository,
            "--draft=false",
        ]
    )
    release = view_release(metadata["tag"], repository)
    if release is None:
        fail(f"release {metadata['tag']} disappeared after publication")
    check_release_identity(release, metadata, release_id)
    if release.get("isDraft"):
        fail(f"release {metadata['tag']} was not published")
    if release.get("isPrerelease") != metadata["prerelease"]:
        fail(f"release {metadata['tag']} has the wrong prerelease flag")
    check_release_assets(release, expected, True)
    verify_remote_tag(metadata, remote)
    print(release.get("url", ""))


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--tag", required=True)
    prepare_parser.add_argument("--tag-object", required=True)
    prepare_parser.add_argument("--tag-commit", required=True)
    prepare_parser.add_argument("--head-commit", required=True)
    prepare_parser.add_argument("--repo-root", default=".")
    prepare_parser.add_argument("--artifacts", required=True)
    prepare_parser.add_argument("--output", required=True)
    prepare_parser.add_argument("--github-output")

    publish_parser = subparsers.add_parser("publish")
    publish_parser.add_argument("--metadata", required=True)
    publish_parser.add_argument("--repository", required=True)
    publish_parser.add_argument(
        "--remote",
        help="git remote URL to verify; defaults to the exact GitHub repository",
    )
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    if args.command == "prepare":
        prepare(args)
    else:
        publish(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

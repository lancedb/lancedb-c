# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Resolve the lancedb-c release version from Git tags or an explicit override."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

DEV_VERSION = "0.0.0-dev"

_NUMERIC_IDENTIFIER = r"(?:0|[1-9][0-9]*)"
_PRERELEASE_IDENTIFIER = r"(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
_PRERELEASE = rf"(?:-{_PRERELEASE_IDENTIFIER}(?:\.{_PRERELEASE_IDENTIFIER})*)?"
_BUILD_IDENTIFIER = r"[0-9A-Za-z-]+"
_BUILD = rf"(?:\+{_BUILD_IDENTIFIER}(?:\.{_BUILD_IDENTIFIER})*)?"
_SEMVER = (
    rf"{_NUMERIC_IDENTIFIER}\.{_NUMERIC_IDENTIFIER}\.{_NUMERIC_IDENTIFIER}"
    rf"{_PRERELEASE}{_BUILD}"
)
_TAG_PATTERN = re.compile(rf"^v(?P<version>{_SEMVER})$", re.ASCII)
_VERSION_PATTERN = re.compile(rf"^{_SEMVER}$", re.ASCII)


def parse_tag(tag: str) -> str | None:
    match = _TAG_PATTERN.fullmatch(tag)
    if not match:
        return None
    version = match.group("version")
    if version == DEV_VERSION:
        return None
    return version


def parse_override(value: str) -> str:
    return _validate_release(value, "LANCEDB_VERSION")


def parse_resolved(value: str) -> str:
    if value == DEV_VERSION:
        return value
    return _validate_release(value, "LANCEDB_RESOLVED_VERSION")


def _validate_release(value: str, source: str) -> str:
    if value.startswith("v"):
        raise ValueError(f"{source} must not include the v tag prefix")
    if value == DEV_VERSION:
        raise ValueError(f"{source} {DEV_VERSION} is reserved for untagged builds")
    if not _VERSION_PATTERN.fullmatch(value):
        raise ValueError(f"{source} is not strict SemVer: {value!r}")
    return value


def is_stable(version: str) -> bool:
    without_build = version.split("+", 1)[0]
    return "-" not in without_build


def _git(repo_root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo_root), *args],
        text=True,
        capture_output=True,
        check=False,
    )


def _tags_at_head(repo_root: Path) -> list[str]:
    result = _git(repo_root, "tag", "--points-at", "HEAD", "--sort=version:refname")
    if result.returncode != 0:
        return []
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def _peeled_commit(repo_root: Path, tag: str) -> str | None:
    result = _git(repo_root, "rev-parse", f"refs/tags/{tag}^{{}}")
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _head_commit(repo_root: Path) -> str | None:
    result = _git(repo_root, "rev-parse", "HEAD")
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def resolve_version(repo_root: Path) -> str:
    # CMake passes its configure-time result to the docs target through this
    # private variable. It takes precedence so a later build-time public
    # override cannot change the already-configured version.
    resolved = os.environ.get("LANCEDB_RESOLVED_VERSION", "").strip()
    if resolved:
        return parse_resolved(resolved)

    override = os.environ.get("LANCEDB_VERSION", "").strip()
    if override:
        return parse_override(override)

    repo_root = Path(repo_root)
    if not (repo_root / ".git").exists():
        return DEV_VERSION

    try:
        head = _head_commit(repo_root)
        tags = _tags_at_head(repo_root)
    except FileNotFoundError:
        return DEV_VERSION
    if not head:
        return DEV_VERSION

    valid: list[tuple[str, str]] = []
    for tag in tags:
        version = parse_tag(tag)
        if version is None:
            continue
        if _peeled_commit(repo_root, tag) != head:
            continue
        valid.append((tag, version))

    if not valid:
        return DEV_VERSION

    stable = [version for _, version in valid if is_stable(version)]
    return (stable or [version for _, version in valid])[-1]


def validate_tag(tag: str) -> str:
    version = parse_tag(tag)
    if version is None:
        raise ValueError(
            f"invalid release tag {tag!r}; expected v<strict SemVer> "
            "without leading zeroes or reserved 0.0.0-dev"
        )
    return version


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    tag_parser = subparsers.add_parser("validate-tag", help="validate a release tag")
    tag_parser.add_argument("tag")
    resolve_parser = subparsers.add_parser(
        "resolve", help="resolve the version from a repository"
    )
    resolve_parser.add_argument("repo_root", nargs="?", default=".")

    args = parser.parse_args(argv)
    try:
        if args.command == "validate-tag":
            print(validate_tag(args.tag))
        else:
            print(resolve_version(Path(args.repo_root)))
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

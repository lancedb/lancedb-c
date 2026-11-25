#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Check clang-format only on lines changed by a Git revision range.

The raw diff supplies the old and new blob IDs that Git already matched, so a
copy is compared with the blob it copied rather than with the source path's
current edits. Identical renames and copies have equal blob IDs and pass; added
files are formatted in full. Paths come from NUL-delimited output and are passed
as arguments, so quoting and non-ASCII names do not affect the check.
"""
from __future__ import annotations

import argparse
import difflib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterator, List, Optional, Sequence, Tuple

EXTENSIONS = frozenset(
    ".c .h .cc .cp .cpp .c++ .cxx .hh .hpp .hxx .h++ .inl .ipp .tpp".split()
)
HUNK_RE = re.compile(rb"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


class CheckError(RuntimeError):
    pass


def run(args: Sequence[str], repo: Path) -> bytes:
    command = list(args)
    try:
        result = subprocess.run(command, cwd=str(repo), capture_output=True)
    except OSError as error:
        raise CheckError("cannot run {}: {}".format(" ".join(command), error))
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise CheckError(
            "{} failed{}".format(
                " ".join(command),
                ": " + detail if detail else "",
            )
        )
    return result.stdout


def changes(repo: Path, base: str) -> Iterator[Tuple[str, str, str, bool]]:
    output = run(
        [
            "git",
            "diff",
            "--raw",
            "-z",
            "--no-abbrev",
            "--no-ext-diff",
            "--no-textconv",
            "--no-color",
            "--diff-filter=ACMRT",
            "-M",
            "-C",
            "--find-copies-harder",
            base,
            "HEAD",
            "--",
        ],
        repo,
    )
    fields = iter(output.split(b"\0")[:-1])
    try:
        for header in fields:
            old_mode, new_mode, old_blob, new_blob, status = header.split()
            path = next(fields)
            if status[:1] in (b"C", b"R"):
                path = next(fields)
            if not new_mode.startswith(b"100"):
                continue
            yield (
                os.fsdecode(path),
                os.fsdecode(old_blob),
                os.fsdecode(new_blob),
                old_mode.startswith(b":100"),
            )
    except (StopIteration, ValueError) as error:
        raise CheckError("cannot parse Git raw diff metadata: {}".format(error))


def changed_ranges(repo: Path, old_blob: str, new_blob: str) -> List[Tuple[int, int]]:
    patch = run(
        [
            "git",
            "diff",
            "--no-ext-diff",
            "--no-textconv",
            "--no-color",
            "-U0",
            old_blob,
            new_blob,
            "--",
        ],
        repo,
    )
    ranges: List[Tuple[int, int]] = []
    for line in patch.splitlines():
        match = HUNK_RE.match(line)
        if match is None:
            continue
        start = int(match.group(1))
        count = int(match.group(2)) if match.group(2) is not None else 1
        if count > 0:
            ranges.append((start, start + count - 1))
    return ranges


def print_violation(path: str, before: bytes, after: bytes) -> None:
    sys.stdout.writelines(
        difflib.unified_diff(
            before.decode("utf-8", "replace").splitlines(keepends=True),
            after.decode("utf-8", "replace").splitlines(keepends=True),
            fromfile="{} (before formatting)".format(path),
            tofile="{} (after formatting)".format(path),
        )
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root containing the C/C++ sources",
    )
    parser.add_argument(
        "--base",
        required=True,
        help="commit to compare HEAD against",
    )
    parser.add_argument(
        "--clang-format",
        default=shutil.which("clang-format"),
        help="clang-format executable",
    )
    args = parser.parse_args(argv)

    repo = args.repo.resolve()
    base = args.base.strip()
    if not base.strip("0"):
        print("no base commit is available; skipping the diff-scoped format check")
        return 0

    clang_format = shutil.which(args.clang_format) if args.clang_format else None
    if not clang_format:
        print(
            "error: clang-format not found: {}".format(args.clang_format),
            file=sys.stderr,
        )
        return 2

    try:
        run(["git", "cat-file", "-e", base + "^{commit}"], repo)
        checked = 0
        failed = False
        for path, old_blob, new_blob, has_old_source in changes(repo, base):
            if Path(path).suffix.lower() not in EXTENSIONS:
                continue
            checked += 1
            if old_blob == new_blob:
                continue
            ranges = changed_ranges(repo, old_blob, new_blob) if has_old_source else []
            if has_old_source and not ranges:
                continue
            try:
                before = (repo / path).read_bytes()
            except OSError as error:
                raise CheckError("cannot read {}: {}".format(path, error))
            command = [clang_format, "--style=file"]
            for start, end in ranges:
                command.append("--lines={}:{}".format(start, end))
            command.extend(["--", path])
            after = run(command, repo)
            if before != after:
                print_violation(path, before, after)
                failed = True

        if checked == 0:
            print("no C or C++ changes to format")
            return 0
        if failed:
            print(
                "C/C++ formatting changes are required on changed lines",
                file=sys.stderr,
            )
            return 1
        print("C/C++ formatting check passed")
        return 0
    except CheckError as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

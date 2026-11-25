#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Check clang-format only on lines changed by a Git revision range.

The check compares the current file with the same file formatted with
``clang-format --lines`` for every hunk that a rename- and copy-aware Git diff
reports. A pure rename or copy has no hunks and therefore passes even when the
existing file contains older formatting differences; a new file has one hunk
covering every line. Paths are passed as arguments and read from NUL-delimited
Git output so spaces and non-ASCII names do not depend on patch quoting.
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
from typing import List, Optional, Sequence, Tuple

EXTENSIONS = frozenset(
    {
        ".c",
        ".h",
        ".cc",
        ".cp",
        ".cpp",
        ".c++",
        ".cxx",
        ".hh",
        ".hpp",
        ".hxx",
        ".h++",
        ".inl",
        ".ipp",
        ".tpp",
    }
)
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


class GitError(RuntimeError):
    pass


class FormatToolError(RuntimeError):
    pass


def run_git(repo: Path, args: Sequence[str], *, env: Optional[dict] = None) -> bytes:
    command = ["git", "-C", str(repo), *args]
    try:
        result = subprocess.run(command, capture_output=True, env=env)
    except OSError as error:
        raise GitError("cannot run {}: {}".format(" ".join(command), error))
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise GitError(
            "{} failed{}".format(
                " ".join(command),
                ": " + detail if detail else "",
            )
        )
    return result.stdout


def changed_files(repo: Path, base: str) -> List[Tuple[str, Optional[str], str]]:
    output = run_git(
        repo,
        [
            "diff",
            "--name-status",
            "-z",
            "--no-ext-diff",
            "--no-textconv",
            "--diff-filter=ACMRC",
            "-M",
            "-C",
            "--find-copies-harder",
            base,
            "HEAD",
        ],
    )
    fields = output.split(b"\0")
    changes: List[Tuple[str, Optional[str], str]] = []
    index = 0
    while index < len(fields):
        if not fields[index]:
            index += 1
            continue
        status = os.fsdecode(fields[index])
        index += 1
        if status[:1] in ("R", "C"):
            if index + 1 >= len(fields):
                raise GitError("truncated name-status output for {}".format(status))
            old = os.fsdecode(fields[index])
            new = os.fsdecode(fields[index + 1])
            index += 2
            changes.append((status, old, new))
        else:
            if index >= len(fields):
                raise GitError("truncated name-status output for {}".format(status))
            path = os.fsdecode(fields[index])
            index += 1
            changes.append((status, None, path))
    return changes


def hunk_ranges(repo: Path, base: str, old: Optional[str], new: str) -> List[Tuple[int, int]]:
    args = [
        "diff",
        "--no-ext-diff",
        "--no-textconv",
        "--no-color",
        "-U0",
        "-M",
        "-C",
        "--find-copies-harder",
        base,
        "HEAD",
        "--",
    ]
    if old is not None:
        args.append(old)
    args.append(new)
    env = os.environ.copy()
    env["GIT_LITERAL_PATHSPECS"] = "1"
    output = run_git(repo, args, env=env)
    ranges: List[Tuple[int, int]] = []
    for line in output.splitlines():
        match = HUNK_RE.match(line.decode("utf-8", "surrogateescape"))
        if match is None:
            continue
        start = int(match.group(1))
        count = int(match.group(2)) if match.group(2) is not None else 1
        if count > 0:
            ranges.append((start, start + count - 1))
    return ranges


def print_violation(path: str, source: bytes, formatted: bytes) -> None:
    before = source.decode("utf-8", "replace").splitlines(keepends=True)
    after = formatted.decode("utf-8", "replace").splitlines(keepends=True)
    sys.stdout.writelines(
        difflib.unified_diff(
            before,
            after,
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
    if not base or set(base) == {"0"}:
        print("no base commit is available; skipping the diff-scoped format check")
        return 0
    clang_format = shutil.which(args.clang_format) if args.clang_format else None
    if not clang_format:
        print("error: clang-format not found: {}".format(args.clang_format), file=sys.stderr)
        return 2

    try:
        run_git(repo, ["cat-file", "-e", base + "^{commit}"])
        changes = changed_files(repo, base)
        selected = [
            (status, old, new)
            for status, old, new in changes
            if Path(new).suffix.lower() in EXTENSIONS
        ]
        if not selected:
            print("no C or C++ changes to format")
            return 0

        violations = 0
        for _status, old, new in selected:
            ranges = hunk_ranges(repo, base, old, new)
            if not ranges:
                continue
            try:
                source = (repo / new).read_bytes()
            except OSError as error:
                raise FormatToolError("cannot read {}: {}".format(new, error))
            for start, end in ranges:
                try:
                    result = subprocess.run(
                        [
                            clang_format,
                            "--style=file",
                            "--lines={}:{}".format(start, end),
                            "--",
                            new,
                        ],
                        cwd=str(repo),
                        capture_output=True,
                    )
                except OSError as error:
                    raise FormatToolError(
                        "cannot run {} for {}: {}".format(clang_format, new, error)
                    )
                if result.returncode != 0:
                    detail = result.stderr.decode("utf-8", "replace").strip()
                    raise FormatToolError(
                        "clang-format failed for {} lines {}:{}{}".format(
                            new,
                            start,
                            end,
                            ": " + detail if detail else "",
                        )
                    )
                if result.stdout != source:
                    print_violation(new, source, result.stdout)
                    violations += 1
                    break

        if violations:
            print(
                "C/C++ formatting changes are required on changed lines",
                file=sys.stderr,
            )
            return 1
        print("C/C++ formatting check passed")
        return 0
    except (GitError, FormatToolError) as error:
        print("error: {}".format(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

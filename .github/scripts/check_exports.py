#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Verify a built shared library against the functions in its public header.

The public header is the single source of truth for the shared-library export
surface. This script reads the installed or packaged header, reads the dynamic
export table of the matching library, and checks that the two sets are equal.
It also loads the library and resolves every public function so a stale or
unloadable artifact cannot pass the check.
"""

from __future__ import annotations

import ctypes
import os
import re
import subprocess
import sys
from pathlib import Path

_FUNCTION_NAME = re.compile(r"\blancedb_[A-Za-z0-9_]+\s*\(")


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            "command failed: {}\nstdout:\n{}\nstderr:\n{}".format(
                " ".join(command), result.stdout, result.stderr
            )
        )
    return result


def _strip_comments(text: str) -> str:
    output: list[str] = []
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            index = text.find("\n", index + 2)
            if index < 0:
                break
            output.append("\n")
            index += 1
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                break
            output.append("\n" * text.count("\n", index, end + 2))
            index = end + 2
        elif text[index] in "\"'":
            quote = text[index]
            start = index
            index += 1
            while index < len(text):
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    index += 1
                    break
                index += 1
            output.append(text[start:index])
        else:
            output.append(text[index])
            index += 1
    return "".join(output)


def _remove_preprocessor_lines(text: str) -> str:
    output: list[str] = []
    continuation = False
    for line in text.splitlines():
        if continuation or line.lstrip().startswith("#"):
            continuation = line.rstrip().endswith("\\")
            output.append("")
        else:
            output.append(line)
    return "\n".join(output)


def _extern_c_blocks(text: str) -> list[str]:
    blocks: list[str] = []
    rest = text
    while True:
        start = rest.find('extern "C"')
        if start < 0:
            break
        after = rest[start + len('extern "C"'):]
        open_brace = after.find("{")
        if open_brace < 0:
            break
        content = after[open_brace + 1:]
        depth = 1
        end = None
        for index, character in enumerate(content):
            if character == "{":
                depth += 1
            elif character == "}":
                depth -= 1
                if depth == 0:
                    end = index
                    break
        if end is None:
            break
        blocks.append(content[:end])
        rest = content[end + 1:]
    return blocks


def _strip_nested_braces(text: str) -> str:
    output: list[str] = []
    depth = 0
    for character in text:
        if character == "{":
            depth += 1
        elif character == "}":
            depth = max(0, depth - 1)
        elif depth == 0:
            output.append(character)
    return "".join(output)


def _split_statements(text: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    parentheses = 0
    brackets = 0
    for character in text:
        if character == "(":
            parentheses += 1
        elif character == ")":
            parentheses -= 1
        elif character == "[":
            brackets += 1
        elif character == "]":
            brackets -= 1
        elif character == ";" and parentheses == 0 and brackets == 0:
            statements.append("".join(current))
            current = []
            continue
        current.append(character)
    if "".join(current).strip():
        statements.append("".join(current))
    return statements


def header_functions(header: Path) -> set[str]:
    text = _remove_preprocessor_lines(
        _strip_comments(header.read_text(encoding="utf-8"))
    )
    functions: set[str] = set()
    for block in _extern_c_blocks(text):
        for statement in _split_statements(_strip_nested_braces(block)):
            statement = statement.strip()
            if not statement or statement.startswith(("static", "typedef")):
                continue
            match = _FUNCTION_NAME.search(statement)
            if match:
                functions.add(match.group(0).split("(", 1)[0].strip())
    if not functions:
        raise SystemExit(f"no public lancedb_ functions found in {header}")
    return functions


def _elf_exports(library: Path) -> set[str]:
    exports: set[str] = set()
    output = run(["readelf", "--wide", "--dyn-syms", str(library)]).stdout
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 8:
            continue
        _, _, _, symbol_type, binding, visibility, section, name = fields[:8]
        if (
            symbol_type == "FUNC"
            and binding in ("GLOBAL", "WEAK")
            and visibility == "DEFAULT"
            and section != "UND"
        ):
            exports.add(name.split("@", 1)[0])
    return exports


def _macho_exports(library: Path) -> set[str]:
    exports: set[str] = set()
    output = run(["nm", "-gUj", str(library)]).stdout
    for line in output.splitlines():
        name = line.strip()
        if name.startswith("_"):
            name = name[1:]
        if name:
            exports.add(name)
    return exports


def _pe_exports(library: Path) -> set[str]:
    output = run(["dumpbin", "/nologo", "/exports", str(library)]).stdout
    exports: set[str] = set()
    for line in output.splitlines():
        match = re.match(
            r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$", line
        )
        if match:
            exports.add(match.group(1))
    function_count = re.search(
        r"number of functions\s+([0-9A-Fa-f]+)", output, re.IGNORECASE
    )
    name_count = re.search(
        r"number of names\s+([0-9A-Fa-f]+)", output, re.IGNORECASE
    )
    if function_count and name_count:
        functions = int(function_count.group(1), 16)
        names = int(name_count.group(1), 16)
        if functions != names:
            raise SystemExit(
                f"{library} exports {functions} functions but only {names} names; "
                "ordinal-only exports are not part of the public header"
            )
    return exports


def _load_library(library: Path, target_os: str) -> ctypes.CDLL:
    if target_os == "windows":
        os.add_dll_directory(str(library.parent))
        return ctypes.WinDLL(str(library))
    return ctypes.CDLL(str(library), mode=ctypes.RTLD_LOCAL)


def verify_library(
    library: Path,
    header: Path,
    target_os: str,
    expected_version: str,
    import_library: Path | None = None,
) -> None:
    expected = header_functions(header)
    if target_os == "linux":
        actual = _elf_exports(library)
    elif target_os == "darwin":
        actual = _macho_exports(library)
    elif target_os == "windows":
        actual = _pe_exports(library)
        if import_library is None or not import_library.is_file():
            raise SystemExit(f"missing Windows import library for {library}")
    else:
        raise SystemExit(f"unsupported export-check platform: {target_os}")

    missing = sorted(expected - actual)
    extra = sorted(actual - expected)
    if missing or extra:
        details = []
        if missing:
            details.append("missing: " + ", ".join(missing))
        if extra:
            details.append("extra: " + ", ".join(extra))
        raise SystemExit(
            f"{library} exports do not match {header}: " + "; ".join(details)
        )

    loaded = _load_library(library, target_os)
    for name in sorted(expected):
        getattr(loaded, name)
    version_function = loaded.lancedb_version
    version_function.restype = ctypes.c_char_p
    runtime_version = version_function()
    if runtime_version is None:
        raise SystemExit(f"{library} returned a NULL version")
    runtime_version = runtime_version.decode("utf-8")
    if runtime_version != expected_version:
        raise SystemExit(
            f"{library} reports version {runtime_version!r}, expected {expected_version!r}"
        )
    print(
        f"export surface OK: {library.name} has {len(expected)} public functions "
        f"and version {runtime_version}"
    )


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print(
            "usage: check_exports.py <library> <header> <linux|darwin|windows> "
            "[expected-version] [import-library]",
            file=sys.stderr,
        )
        return 2
    library = Path(argv[0]).resolve()
    header = Path(argv[1]).resolve()
    target_os = argv[2]
    if target_os == "windows":
        if len(argv) < 5:
            print("expected-version is required", file=sys.stderr)
            return 2
        expected_version = argv[3]
        import_library = Path(argv[4]).resolve() if len(argv) > 4 else None
    else:
        if len(argv) < 4:
            print("expected-version is required", file=sys.stderr)
            return 2
        expected_version = argv[3]
        import_library = None
    verify_library(library, header, target_os, expected_version, import_library)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

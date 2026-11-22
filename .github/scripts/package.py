#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Create and verify relocatable lancedb-c shared-library packages.

The package layout is the contract the later pure-Go loader consumes:

    lancedb-c-<commit>-<target>/
      LICENSE
      include/lancedb.h
      lib/<shared library>
      lib/<import library>       (Windows only)
      manifest.json
      SHA256SUMS

`target` uses the Go toolchain's GOOS-GOARCH names. The manifest records the
Git-tag-derived binding version, commit and packaged files.
"""

from __future__ import annotations

import argparse
import dataclasses
import gzip
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from version import DEV_VERSION, parse_resolved, resolve_version


SCHEMA_VERSION = 1
PACKAGE_NAME = "lancedb-c"
HEADER_VERSION_DEFAULT = (
    "#define LANCEDB_VERSION_MAJOR 0\n"
    "#define LANCEDB_VERSION_MINOR 0\n"
    "#define LANCEDB_VERSION_PATCH 0\n"
    '#define LANCEDB_VERSION_STRING "0.0.0-dev"'
)
_HEADER_VERSION_CORE = re.compile(r"^([0-9]+)[.]([0-9]+)[.]([0-9]+)")


@dataclasses.dataclass(frozen=True)
class Target:
    name: str
    triple: str
    os: str
    arch: str
    library: str
    import_library: str | None = None


TARGETS = {
    "x86_64-unknown-linux-gnu": Target(
        "linux-amd64", "x86_64-unknown-linux-gnu", "linux", "x86_64", "liblancedb.so"
    ),
    "aarch64-unknown-linux-gnu": Target(
        "linux-arm64", "aarch64-unknown-linux-gnu", "linux", "aarch64", "liblancedb.so"
    ),
    "aarch64-apple-darwin": Target(
        "darwin-arm64", "aarch64-apple-darwin", "darwin", "aarch64", "liblancedb.dylib"
    ),
    "x86_64-pc-windows-msvc": Target(
        "windows-amd64",
        "x86_64-pc-windows-msvc",
        "windows",
        "x86_64",
        "lancedb.dll",
        "lancedb.dll.lib",
    ),
    "aarch64-pc-windows-msvc": Target(
        "windows-arm64",
        "aarch64-pc-windows-msvc",
        "windows",
        "aarch64",
        "lancedb.dll",
        "lancedb.dll.lib",
    ),
}


def target_for(value: str) -> Target:
    if value in TARGETS:
        return TARGETS[value]
    for target in TARGETS.values():
        if target.name == value:
            return target
    raise SystemExit(f"unsupported target {value!r}; expected one of {sorted(TARGETS)}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_lf(path: Path, text: str) -> None:
    """Write UTF-8 text with canonical LF newlines, including on Windows."""
    path.write_bytes(text.encode("utf-8"))


def package_version(repo_root: Path) -> str:
    try:
        return resolve_version(repo_root)
    except ValueError as error:
        raise SystemExit(str(error)) from error


def render_header(source: bytes, version: str) -> bytes:
    """Return the public header rendered with a resolved build version."""
    try:
        resolved = parse_resolved(version)
    except ValueError as error:
        raise SystemExit(str(error)) from error
    match = _HEADER_VERSION_CORE.match(resolved)
    if not match:
        raise SystemExit(f"cannot render lancedb-c version {resolved!r} without a numeric core")
    if b"\r" in source:
        raise SystemExit("the public header must use LF line endings")
    try:
        text = source.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SystemExit(f"the public header is not UTF-8: {error}") from error
    occurrences = text.count(HEADER_VERSION_DEFAULT)
    if occurrences != 1:
        raise SystemExit(
            "the public header must contain exactly one default LANCEDB_VERSION block, "
            f"found {occurrences}"
        )
    rendered = (
        f"#define LANCEDB_VERSION_MAJOR {match.group(1)}\n"
        f"#define LANCEDB_VERSION_MINOR {match.group(2)}\n"
        f"#define LANCEDB_VERSION_PATCH {match.group(3)}\n"
        f'#define LANCEDB_VERSION_STRING "{resolved}"'
    )
    output = text.replace(HEADER_VERSION_DEFAULT, rendered).encode("utf-8")
    if b"\r" in output:
        raise SystemExit("the rendered public header must use LF line endings")
    return output


def set_github_output(**values: str) -> None:
    output = os.environ.get("GITHUB_OUTPUT")
    if not output:
        return
    with Path(output).open("a", encoding="utf-8") as file:
        for key, value in values.items():
            file.write(f"{key}={value}\n")


def copy_file(source: Path, destination: Path, mode: int) -> None:
    if not source.is_file():
        raise SystemExit(f"missing package input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)


def write_deterministic_tar(
    source_root: Path, archive: Path, file_modes: dict[str, int]
) -> None:
    """Write a tar.gz with stable metadata so identical inputs give identical bytes."""
    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(
                fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT
            ) as tar:
                for path in sorted(source_root.rglob("*")):
                    relative = path.relative_to(source_root).as_posix()
                    arcname = f"{source_root.name}/{relative}"
                    info = tar.gettarinfo(str(path), arcname=arcname)
                    info.uid = 0
                    info.gid = 0
                    info.uname = ""
                    info.gname = ""
                    info.mtime = 0
                    if path.is_dir():
                        # Do not take directory modes from the caller's umask.
                        info.mode = 0o755
                        tar.addfile(info)
                    else:
                        if relative not in file_modes:
                            raise SystemExit(f"no deterministic mode for package member: {relative}")
                        info.mode = file_modes[relative]
                        with path.open("rb") as file:
                            tar.addfile(info, file)


def extract_tar(archive: Path, destination: Path) -> None:
    destination = destination.resolve()
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            if not (member.isfile() or member.isdir()):
                raise SystemExit(f"unsupported archive member: {member.name}")
            target = (destination / member.name).resolve()
            if target != destination and destination not in target.parents:
                raise SystemExit(f"archive member escapes destination: {member.name}")
            tar.extract(member, destination)


def package_manifest(target: Target, commit: str, version: str) -> dict:
    return {
        "schemaVersion": SCHEMA_VERSION,
        "name": PACKAGE_NAME,
        "version": version,
        "target": target.name,
        "rustTarget": target.triple,
        "commit": commit,
        "library": f"lib/{target.library}",
        "include": "include/lancedb.h",
        "importLibrary": f"lib/{target.import_library}" if target.import_library else None,
        "license": "LICENSE",
    }


def create_package(args: argparse.Namespace) -> None:
    target = target_for(args.target)
    repo_root = Path(args.repo_root).resolve()
    output_dir = Path(args.output_dir).resolve()
    library_dir = (
        Path(args.library_dir).resolve()
        if args.library_dir
        else repo_root / "target" / target.triple / "release"
    )

    required_libraries = [library_dir / target.library]
    if target.import_library:
        required_libraries.append(library_dir / target.import_library)
    for library in required_libraries:
        if not library.is_file():
            raise SystemExit(f"missing build output: {library}")

    output_dir.mkdir(parents=True, exist_ok=True)
    version = package_version(repo_root)
    package_root_name = f"{PACKAGE_NAME}-{args.commit}-{target.name}"
    with tempfile.TemporaryDirectory(prefix="lancedb-c-package-") as temp:
        package_root = Path(temp) / package_root_name
        copy_file(repo_root / "LICENSE", package_root / "LICENSE", 0o644)
        header = render_header(
            (repo_root / "include" / "lancedb.h").read_bytes(), version
        )
        (package_root / "include").mkdir(parents=True, exist_ok=True)
        (package_root / "include" / "lancedb.h").write_bytes(header)
        copy_file(required_libraries[0], package_root / "lib" / target.library, 0o755)
        if target.import_library:
            copy_file(
                required_libraries[1],
                package_root / "lib" / target.import_library,
                0o644,
            )

        manifest = package_manifest(target, args.commit, version)
        write_lf(package_root / "manifest.json", json.dumps(manifest, indent=2) + "\n")

        checksummed_files = [
            path
            for path in sorted(package_root.rglob("*"))
            if path.is_file() and path.name != "SHA256SUMS"
        ]
        checksum_lines = [
            f"{sha256_file(path)}  {path.relative_to(package_root).as_posix()}"
            for path in checksummed_files
        ]
        write_lf(package_root / "SHA256SUMS", "\n".join(checksum_lines) + "\n")

        file_modes = {
            "LICENSE": 0o644,
            "include/lancedb.h": 0o644,
            "manifest.json": 0o644,
            "SHA256SUMS": 0o644,
            manifest["library"]: 0o755,
        }
        if manifest["importLibrary"]:
            file_modes[manifest["importLibrary"]] = 0o644

        archive = output_dir / f"{package_root_name}.tar.gz"
        write_deterministic_tar(package_root, archive, file_modes)

    digest = sha256_file(archive)
    checksum_file = output_dir / f"{archive.name}.sha256"
    write_lf(checksum_file, f"{digest}  {archive.name}\n")

    print(json.dumps({"archive": str(archive), "sha256": digest, "target": target.name}, indent=2))
    set_github_output(
        archive=str(archive),
        checksum=str(checksum_file),
        sha256=digest,
        target=target.name,
    )


def read_elf_arch(path: Path) -> str:
    with path.open("rb") as file:
        data = file.read(64)
    if data[:4] != b"\x7fELF" or len(data) < 20:
        raise SystemExit(f"{path} is not an ELF file")
    endian = "<" if data[5] == 1 else ">"
    machine = struct.unpack_from(endian + "H", data, 18)[0]
    return {0x3E: "x86_64", 0xB7: "aarch64"}.get(machine, f"elf-machine-0x{machine:x}")


def read_macho_arch(path: Path) -> str:
    with path.open("rb") as file:
        data = file.read(8)
    if len(data) < 8:
        raise SystemExit(f"{path} is not a Mach-O file")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != 0xFEEDFACF:
        raise SystemExit(f"{path} is not a 64-bit little-endian Mach-O file")
    cpu = struct.unpack_from("<I", data, 4)[0]
    return {0x01000007: "x86_64", 0x0100000C: "aarch64"}.get(cpu, f"macho-cpu-0x{cpu:x}")


def read_pe_arch(path: Path) -> str:
    with path.open("rb") as file:
        data = file.read(4096)
    if data[:2] != b"MZ":
        raise SystemExit(f"{path} is not a PE file")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_offset : pe_offset + 4] != b"PE\x00\x00":
        raise SystemExit(f"{path} has no PE signature")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    return {0x8664: "x86_64", 0xAA64: "aarch64"}.get(machine, f"pe-machine-0x{machine:x}")


def binary_arch(target: Target, library: Path) -> str:
    if target.os == "linux":
        return read_elf_arch(library)
    if target.os == "darwin":
        return read_macho_arch(library)
    return read_pe_arch(library)


def run(command: list[str], env: dict | None = None, show: bool = True) -> subprocess.CompletedProcess:
    print("+ " + " ".join(command))
    completed = subprocess.run(command, text=True, capture_output=True, env=env)
    if show and completed.stdout:
        print(completed.stdout.rstrip())
    if completed.stderr:
        print(completed.stderr.rstrip(), file=sys.stderr)
    if completed.returncode != 0:
        raise SystemExit(f"command failed with exit code {completed.returncode}: {command[0]}")
    return completed


def glibc_version_key(version: str) -> tuple[int, ...]:
    return tuple(int(part) for part in version.removeprefix("GLIBC_").split("."))


def parse_macho_install_name(otool_output: str) -> str | None:
    lines = otool_output.splitlines()
    for index, line in enumerate(lines):
        if line.strip() != "cmd LC_ID_DYLIB":
            continue
        for candidate in lines[index + 1 : index + 12]:
            stripped = candidate.strip()
            if not stripped.startswith("name "):
                continue
            value = stripped.removeprefix("name ")
            if " (offset " in value:
                value = value.split(" (offset ", 1)[0]
            return value.strip()
        return None
    return None


def read_macho_install_name(library: Path) -> str:
    output = run(["otool", "-l", str(library)], show=False).stdout
    install_name = parse_macho_install_name(output)
    if not install_name:
        raise SystemExit(f"could not read LC_ID_DYLIB from {library}")
    return install_name


def print_runtime_dependencies(target: Target, library: Path) -> None:
    if target.os == "linux":
        run(["file", str(library)])
        run(["readelf", "-d", str(library)])
        ldd = run(["ldd", str(library)]).stdout
        if "not found" in ldd:
            raise SystemExit(f"{library} has unresolved runtime dependencies")
        versions = set(
            re.findall(r"GLIBC_[0-9.]+", run(["readelf", "--version-info", str(library)]).stdout)
        )
        if versions:
            print("Required GLIBC versions: " + ", ".join(sorted(versions, key=glibc_version_key)))
    elif target.os == "darwin":
        run(["file", str(library)])
        install_name = read_macho_install_name(library)
        print("Install name: " + install_name)
        if install_name != "@rpath/liblancedb.dylib":
            raise SystemExit(
                f"{library} has install name {install_name!r}, expected "
                "'@rpath/liblancedb.dylib' so the package stays relocatable"
            )
        run(["otool", "-L", str(library)])
    else:
        headers = run(["dumpbin", "/headers", str(library)], show=False).stdout
        for line in headers.splitlines():
            if "machine" in line.lower():
                print(line.strip())
        run(["dumpbin", "/dependents", str(library)])


def find_package_root(work_dir: Path, expected_name: str) -> Path:
    candidates = [path for path in work_dir.iterdir() if path.is_dir()]
    roots = [path for path in candidates if path.name == expected_name]
    if len(roots) != 1:
        raise SystemExit(f"expected package root {expected_name!r}, found {[p.name for p in candidates]}")
    return roots[0]


def read_checksum_inventory(package_root: Path) -> dict[str, str]:
    checksum_file = package_root / "SHA256SUMS"
    if not checksum_file.is_file():
        raise SystemExit("package has no SHA256SUMS")
    entries: dict[str, str] = {}
    for line_number, line in enumerate(checksum_file.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        if "  " not in line:
            raise SystemExit(f"SHA256SUMS line {line_number} is not '<sha256>  <path>'")
        expected, relative = line.split("  ", 1)
        if relative in entries:
            raise SystemExit(f"SHA256SUMS lists {relative} more than once")
        entries[relative] = expected
    return entries


def verify_checksums(package_root: Path) -> None:
    entries = read_checksum_inventory(package_root)
    expected_paths = {
        path.relative_to(package_root).as_posix()
        for path in package_root.rglob("*")
        if path.is_file() and path.name != "SHA256SUMS"
    }
    missing = sorted(expected_paths - entries.keys())
    extra = sorted(entries.keys() - expected_paths)
    if missing:
        raise SystemExit(f"SHA256SUMS is missing: {', '.join(missing)}")
    if extra:
        raise SystemExit(f"SHA256SUMS lists unexpected entries: {', '.join(extra)}")
    for relative, expected in entries.items():
        path = package_root / relative
        actual = sha256_file(path)
        if actual != expected:
            raise SystemExit(f"checksum mismatch for {relative}: {actual} != {expected}")


def build_and_run_consumer(
    target: Target,
    package_root: Path,
    source: Path,
    work_dir: Path,
    version: str,
) -> None:
    if not source.is_file():
        raise SystemExit(f"missing consumer source: {source}")
    uri = work_dir / "database"
    if target.os in ("linux", "darwin"):
        executable = work_dir / "package-consumer"
        command = [
            os.environ.get("CXX", "c++"),
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{package_root / 'include'}",
            str(source),
            f"-L{package_root / 'lib'}",
            "-llancedb",
            f"-Wl,-rpath,{package_root / 'lib'}",
            "-o",
            str(executable),
        ]
        run(command)
        if target.os == "linux":
            consumer_ldd = run(["ldd", str(executable)]).stdout
            if str(package_root / "lib") not in consumer_ldd:
                raise SystemExit("the consumer does not resolve liblancedb from the package")
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = f"{package_root / 'lib'}{os.pathsep}{env.get('LD_LIBRARY_PATH', '')}"
        env["DYLD_LIBRARY_PATH"] = f"{package_root / 'lib'}{os.pathsep}{env.get('DYLD_LIBRARY_PATH', '')}"
        run([str(executable), str(uri), version], env=env)
    else:
        executable = work_dir / "package-consumer.exe"
        command = [
            "cl",
            "/nologo",
            "/std:c++17",
            "/EHsc",
            "/W4",
            f"/I{package_root / 'include'}",
            str(source),
            f"/Fe:{executable}",
            "/link",
            f"/LIBPATH:{package_root / 'lib'}",
            target.import_library,
        ]
        run(command)
        env = os.environ.copy()
        env["PATH"] = f"{package_root / 'lib'}{os.pathsep}{env.get('PATH', '')}"
        run([str(executable), str(uri), version], env=env)


def verify_archive_checksum(archive: Path, checksum_file: Path) -> str:
    if not checksum_file.is_file():
        raise SystemExit(f"missing archive checksum: {checksum_file}")
    checksum_fields = checksum_file.read_text(encoding="utf-8").split()
    if len(checksum_fields) != 2:
        raise SystemExit(f"invalid archive checksum file: {checksum_file}")
    expected_digest, checksum_name = checksum_fields
    if checksum_name != archive.name:
        raise SystemExit(
            f"archive checksum names {checksum_name!r}, expected {archive.name!r}"
        )
    actual_digest = sha256_file(archive)
    if actual_digest != expected_digest:
        raise SystemExit(f"archive checksum mismatch: {actual_digest} != {expected_digest}")
    return actual_digest


def verify_package(args: argparse.Namespace) -> None:
    target = target_for(args.target)
    repo_root = Path(args.repo_root).resolve()
    archive = Path(args.archive).resolve()
    if not archive.is_file():
        raise SystemExit(f"missing archive: {archive}")

    actual_digest = verify_archive_checksum(archive, Path(args.checksum).resolve())
    print(f"Archive SHA256: {actual_digest}")

    with tempfile.TemporaryDirectory(prefix="lancedb-c-verify-") as temp:
        work_dir = Path(temp)
        extract_tar(archive, work_dir)
        package_root_name = f"{PACKAGE_NAME}-{args.commit}-{target.name}"
        package_root = find_package_root(work_dir, package_root_name)

        manifest_path = package_root / "manifest.json"
        if not manifest_path.is_file():
            raise SystemExit("package has no manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        expected = package_manifest(target, args.commit, package_version(repo_root))
        if manifest != expected:
            raise SystemExit(f"manifest mismatch:\n{json.dumps(manifest, indent=2)}\n!=\n{json.dumps(expected, indent=2)}")

        for relative in [manifest["library"], manifest["include"], manifest["license"]]:
            if not (package_root / relative).is_file():
                raise SystemExit(f"manifest entry is missing: {relative}")
        if manifest["importLibrary"] and not (package_root / manifest["importLibrary"]).is_file():
            raise SystemExit(f"manifest import library is missing: {manifest['importLibrary']}")

        package_header = package_root / manifest["include"]
        expected_header = render_header(
            (repo_root / "include" / "lancedb.h").read_bytes(), manifest["version"]
        )
        if package_header.read_bytes() != expected_header:
            raise SystemExit(
                "packaged lancedb.h is not the rendered header for its manifest version"
            )

        verify_checksums(package_root)

        library = package_root / manifest["library"]
        actual_arch = binary_arch(target, library)
        if actual_arch != target.arch:
            raise SystemExit(f"library architecture is {actual_arch}, expected {target.arch}")
        print(f"Library architecture: {actual_arch}")

        print_runtime_dependencies(target, library)
        consumer_source = Path(args.consumer_source)
        if not consumer_source.is_absolute():
            consumer_source = repo_root / consumer_source
        build_and_run_consumer(
            target, package_root, consumer_source.resolve(), work_dir, manifest["version"]
        )

    print(f"package verification OK: target={target.name}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create", help="create a package from a cargo build")
    create.add_argument("--target", required=True)
    create.add_argument("--commit", required=True)
    create.add_argument("--repo-root", default=".")
    create.add_argument("--library-dir", help="defaults to target/<triple>/release")
    create.add_argument("--output-dir", required=True)

    verify = subparsers.add_parser("verify", help="verify an unpacked package with a real consumer")
    verify.add_argument("--target", required=True)
    verify.add_argument("--commit", required=True)
    verify.add_argument("--repo-root", default=".")
    verify.add_argument("--archive", required=True)
    verify.add_argument("--checksum", required=True)
    verify.add_argument(
        "--consumer-source", default="tests/package_consumer.cpp"
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.command == "create":
        create_package(args)
    else:
        verify_package(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

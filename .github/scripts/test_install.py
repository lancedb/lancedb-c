#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Install a configured build and verify find_package consumers.

This checks the installed layout rather than the build tree: it stages the
current build under a temporary prefix, configures small C and C++ consumers
with ``find_package(lancedb CONFIG REQUIRED)``, then builds and runs both.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def cache_value(build_dir: Path, name: str) -> str:
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    match = re.search(rf"^{re.escape(name)}:[^=]*=(.*)$", cache, re.MULTILINE)
    return match.group(1).strip() if match else ""


def run(command: list[str], env: dict[str, str] | None = None) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, env=env, check=True)


def find_executable(build_dir: Path, name: str, config: str) -> Path:
    executable = name + (".exe" if os.name == "nt" else "")
    candidates = sorted(build_dir.rglob(executable))
    if config:
        preferred = [path for path in candidates if config in path.parts]
        if preferred:
            candidates = preferred
    if len(candidates) != 1:
        raise SystemExit(
            f"expected one {executable} in {build_dir}, found "
            f"{[str(path) for path in candidates]}"
        )
    return candidates[0]


def prepend(env: dict[str, str], name: str, directory: Path) -> None:
    env[name] = f"{directory}{os.pathsep}{env.get(name, '')}"


def installed_version(prefix: Path) -> str:
    header = (prefix / "include" / "lancedb.h").read_text(encoding="utf-8")
    match = re.search(r'^#define LANCEDB_VERSION_STRING "([^"]+)"$', header, re.MULTILINE)
    if not match:
        raise SystemExit("installed header has no LANCEDB_VERSION_STRING")
    return match.group(1)


def configure_version_case(
    source_dir: Path,
    work_dir: Path,
    package_dir: Path,
    generator: str,
    generator_platform: str,
    requested: str,
    exact: bool,
    expect_success: bool,
) -> None:
    name = requested or "unversioned"
    if exact:
        name += "-exact"
    build_dir = work_dir / f"version-{name.replace('.', '-')}"
    command = [
        "cmake",
        "-S",
        str(source_dir / "tests" / "find_package_version"),
        "-B",
        str(build_dir),
        f"-Dlancedb_DIR={package_dir}",
        f"-DLANCEDB_FIND_VERSION={requested}",
    ]
    if exact:
        command.append("-DLANCEDB_FIND_EXACT=ON")
    if generator:
        command.extend(["-G", generator])
    if generator_platform:
        command.extend(["-A", generator_platform])
    print("+ " + " ".join(command))
    completed = subprocess.run(command, text=True, capture_output=True)
    if expect_success and completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr, file=sys.stderr)
        raise SystemExit(f"version request {requested!r} should be accepted")
    if not expect_success:
        if completed.returncode == 0:
            print(completed.stdout)
            raise SystemExit(f"version request {requested!r} should be rejected")
        print(f"rejected as expected: {requested!r}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--config", default="")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    build_dir = Path(args.build_dir).resolve()
    source_dir = Path(args.source_dir).resolve()
    config = args.config.strip()

    generator = cache_value(build_dir, "CMAKE_GENERATOR")
    generator_platform = cache_value(build_dir, "CMAKE_GENERATOR_PLATFORM")
    libdir = cache_value(build_dir, "CMAKE_INSTALL_LIBDIR") or "lib"
    bindir = cache_value(build_dir, "CMAKE_INSTALL_BINDIR") or "bin"
    build_type = cache_value(build_dir, "CMAKE_BUILD_TYPE") or config or "Release"

    work_dir = build_dir / "check-install"
    shutil.rmtree(work_dir, ignore_errors=True)
    work_dir.mkdir(parents=True)
    prefix = work_dir / "prefix"

    install = [
        "cmake",
        "--install",
        str(build_dir),
        "--prefix",
        str(prefix),
    ]
    if config:
        install.extend(["--config", config])
    run(install)

    installed_libdir = Path(libdir) if os.path.isabs(libdir) else prefix / libdir
    installed_bindir = Path(bindir) if os.path.isabs(bindir) else prefix / bindir
    package_dir = installed_libdir / "cmake" / "lancedb"
    if not (package_dir / "lancedbConfig.cmake").is_file():
        raise SystemExit(f"installed package config is missing: {package_dir}")

    consumer_source = source_dir / "tests" / "find_package_consumer"
    consumer_build = work_dir / "consumer-build"
    configure = [
        "cmake",
        "-S",
        str(consumer_source),
        "-B",
        str(consumer_build),
        f"-Dlancedb_DIR={package_dir}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
    ]
    if generator:
        configure.extend(["-G", generator])
    if generator_platform:
        configure.extend(["-A", generator_platform])
    run(configure)

    build = ["cmake", "--build", str(consumer_build), "--parallel", "4"]
    if config:
        build.extend(["--config", config])
    run(build)

    env = os.environ.copy()
    prepend(env, "PATH", installed_bindir)
    prepend(env, "PATH", installed_libdir)
    prepend(env, "LD_LIBRARY_PATH", installed_libdir)
    prepend(env, "DYLD_LIBRARY_PATH", installed_libdir)

    c_consumer = find_executable(
        consumer_build, "lancedb_c_find_package_consumer", config
    )
    cxx_consumer = find_executable(
        consumer_build, "lancedb_cxx_find_package_consumer", config
    )
    run([str(c_consumer), str(work_dir / "c-consumer-db")], env=env)
    run([str(cxx_consumer), str(work_dir / "cxx-consumer-db")], env=env)

    version = installed_version(prefix)
    configure_version_case(
        source_dir, work_dir, package_dir, generator, generator_platform, "", False, True
    )
    numeric_core = version.split("+", 1)[0].split("-", 1)[0]
    if "-" in version.split("+", 1)[0]:
        # CMake cannot parse prerelease request syntax, so every numeric request
        # must fail rather than selecting the preview as its stable core.
        configure_version_case(
            source_dir,
            work_dir,
            package_dir,
            generator,
            generator_platform,
            numeric_core,
            False,
            False,
        )
        major_minor = ".".join(numeric_core.split(".")[:2])
        configure_version_case(
            source_dir,
            work_dir,
            package_dir,
            generator,
            generator_platform,
            major_minor,
            False,
            False,
        )
        configure_version_case(
            source_dir,
            work_dir,
            package_dir,
            generator,
            generator_platform,
            f"{numeric_core}...{numeric_core}",
            False,
            False,
        )
    else:
        # Exact requests cannot express build metadata. Pure stable versions
        # keep the standard EXACT path; versions with metadata use their
        # numeric core through the normal compatibility check.
        exact_version = version if "+" not in version else numeric_core
        configure_version_case(
            source_dir,
            work_dir,
            package_dir,
            generator,
            generator_platform,
            exact_version,
            "+" not in version,
            True,
        )
    print("installed package consumers OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

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
    print("installed package consumers OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
"""Install a configured build and verify installed package consumers.

This checks the installed layout rather than the build tree: it stages the
current build under a temporary prefix, configures small C and C++ consumers
with ``find_package(lancedb CONFIG REQUIRED)``, then builds and runs both. On
Unix it also compiles, links and runs a C consumer through the installed
``lancedb.pc`` using ``pkg-config`` (``--static`` for static builds).
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

from check_exports import verify_library


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


def build_is_shared(build_dir: Path) -> bool:
    value = cache_value(build_dir, "BUILD_SHARED_LIBS").strip().upper()
    return value not in ("OFF", "0", "FALSE", "NO")


def pkg_config_output(pkg_config: str, env: dict[str, str], *arguments: str) -> str:
    command = [pkg_config, *arguments, "lancedb"]
    completed = subprocess.run(command, env=env, text=True, capture_output=True)
    if completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr, file=sys.stderr)
        raise SystemExit(f"pkg-config {' '.join(arguments)} lancedb failed")
    return completed.stdout.strip()


def verify_pkg_config_consumer(
    source_dir: Path,
    work_dir: Path,
    prefix: Path,
    installed_libdir: Path,
    shared: bool,
) -> None:
    """Compile, link and run the C17 consumer through the installed .pc file.

    This is a Unix check: the probe consumes only the pkg-config flags. It
    restricts pkg-config to the staged directory and drops inherited loader
    paths, so a missing or broken staged file cannot fall back to a system
    installation, and the binary must load the staged library through the
    reported libdir alone.
    """
    pkg_config = shutil.which("pkg-config") or shutil.which("pkgconf")
    if pkg_config is None:
        raise SystemExit("pkg-config or pkgconf is required to verify lancedb.pc")

    pkgconfig_dir = installed_libdir / "pkgconfig"
    pc_file = pkgconfig_dir / "lancedb.pc"
    if not pc_file.is_file():
        raise SystemExit(f"installed pkg-config file is missing: {pc_file}")

    env = os.environ.copy()
    env["PKG_CONFIG_PATH"] = str(pkgconfig_dir)
    env["PKG_CONFIG_LIBDIR"] = str(pkgconfig_dir)
    env.pop("PKG_CONFIG_SYSROOT_DIR", None)
    env.pop("LD_LIBRARY_PATH", None)
    env.pop("DYLD_LIBRARY_PATH", None)

    # The .pc file derives its prefix from its own location, so a staged or
    # relocated installation must report the staged prefix here.
    reported_prefix = Path(pkg_config_output(pkg_config, env, "--variable=prefix"))
    if reported_prefix.resolve() != prefix.resolve():
        raise SystemExit(
            f"pkg-config reports prefix {reported_prefix}, expected {prefix.resolve()}"
        )
    reported_libdir = Path(pkg_config_output(pkg_config, env, "--variable=libdir"))

    lib_arguments = ["--libs"]
    if not shared:
        lib_arguments.append("--static")
    cflags = shlex.split(pkg_config_output(pkg_config, env, "--cflags"))
    libs = shlex.split(pkg_config_output(pkg_config, env, *lib_arguments))

    compiler = os.environ.get("CC", "cc")
    consumer_binary = work_dir / "pkg-config-consumer"
    compile_command = [
        compiler,
        "-std=c17",
        *cflags,
        str(source_dir / "tests" / "c17_consumer.c"),
        "-o",
        str(consumer_binary),
        *libs,
        f"-Wl,-rpath,{reported_libdir}",
    ]
    run(compile_command, env=env)
    run([str(consumer_binary), str(work_dir / "pkg-config-consumer-db")], env=env)


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
    parser.add_argument(
        "--export-target",
        default="",
        choices=("", "linux", "darwin", "windows"),
        help="CMake-selected export verifier target; empty disables the check",
    )
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

    if os.name != "nt":
        verify_pkg_config_consumer(
            source_dir,
            work_dir,
            prefix,
            installed_libdir,
            build_is_shared(build_dir),
        )

    version = installed_version(prefix)
    target_os = args.export_target or None
    if target_os is not None:
        header_path = prefix / "include" / "lancedb.h"
        if target_os == "windows":
            library = installed_bindir / "lancedb.dll"
            import_library = installed_libdir / "lancedb.dll.lib"
        elif target_os == "darwin":
            library = installed_libdir / "liblancedb.dylib"
            import_library = None
        else:
            library = installed_libdir / "liblancedb.so"
            import_library = None
        verify_library(
            library,
            header_path,
            target_os,
            version,
            import_library,
        )
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

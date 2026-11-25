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


def resolve_c_compiler(build_dir: Path | None) -> list[str]:
    """Resolve the C compiler command recorded for the build.

    CMake stores the command in CMAKE_C_COMPILER and arguments such as -m64
    in CMAKE_C_COMPILER_ARG1. A build directory without recorded compilers
    and direct callers fall back to the standard CC environment variable.
    """
    if build_dir is not None:
        compiler = cache_value(build_dir, "CMAKE_C_COMPILER")
        if compiler:
            arguments = shlex.split(cache_value(build_dir, "CMAKE_C_COMPILER_ARG1"))
            return [compiler, *arguments]
    configured = os.environ.get("CC", "").strip()
    return shlex.split(configured) if configured else ["cc"]


def pkg_config_output(pkg_config: str, env: dict[str, str], *arguments: str) -> str:
    command = [pkg_config, *arguments, "lancedb"]
    completed = subprocess.run(command, env=env, text=True, capture_output=True)
    if completed.returncode != 0:
        print(completed.stdout)
        print(completed.stderr, file=sys.stderr)
        raise SystemExit(f"pkg-config {' '.join(arguments)} lancedb failed")
    return completed.stdout.strip()


def pkg_config_variable(pkg_config: str, env: dict[str, str], name: str) -> Path:
    # pkg-config shell-escapes variable values, so a staged path containing
    # whitespace arrives escaped and must be split like the flag output.
    value = pkg_config_output(pkg_config, env, f"--variable={name}")
    parsed = shlex.split(value)
    if len(parsed) != 1:
        raise SystemExit(f"pkg-config reports an unparsable {name}: {value!r}")
    return Path(parsed[0])


def flag_search_directories(flags: list[str]) -> list[Path]:
    directories: list[Path] = []
    index = 0
    while index < len(flags):
        flag = flags[index]
        if flag in ("-I", "-isystem", "-L") and index + 1 < len(flags):
            directories.append(Path(flags[index + 1]))
            index += 2
            continue
        for option in ("-isystem", "-I", "-L"):
            if flag.startswith(option) and len(flag) > len(option):
                directories.append(Path(flag[len(option) :]))
                break
        index += 1
    return directories


def is_within(candidate: Path, root: Path) -> bool:
    candidate = candidate.resolve()
    root = root.resolve()
    return candidate == root or root in candidate.parents


def check_shared_library_origin(
    binary: Path, env: dict[str, str], libdir: Path
) -> None:
    """Fail unless the consumer resolves lancedb from the staged libdir."""
    expected = Path(os.path.normpath(libdir))
    if sys.platform == "darwin":
        names = subprocess.run(
            ["otool", "-L", str(binary)], env=env, text=True, capture_output=True
        )
        if names.returncode != 0:
            raise SystemExit(f"otool -L failed for {binary}:\n{names.stderr}")
        install_name = ""
        for line in names.stdout.splitlines():
            entry = line.strip()
            if "liblancedb" in entry:
                install_name = entry.split(" (", 1)[0].strip()
                break
        if not install_name:
            raise SystemExit(f"{binary} does not link an installed lancedb library")
        if install_name.startswith("@rpath/"):
            details = subprocess.run(
                ["otool", "-l", str(binary)], env=env, text=True, capture_output=True
            )
            if details.returncode != 0:
                raise SystemExit(f"otool -l failed for {binary}:\n{details.stderr}")
            rpaths = []
            lines = details.stdout.splitlines()
            for index, line in enumerate(lines):
                if line.strip() != "cmd LC_RPATH":
                    continue
                for candidate in lines[index + 1 : index + 4]:
                    candidate = candidate.strip()
                    if candidate.startswith("path "):
                        rpath = candidate[len("path ") :].split(" (offset", 1)[0]
                        rpaths.append(Path(os.path.normpath(rpath.strip())))
                        break
            if expected not in rpaths:
                raise SystemExit(
                    f"{binary} resolves lancedb through {rpaths}, expected {expected}"
                )
            return
        if Path(os.path.normpath(install_name)).parent != expected:
            raise SystemExit(
                f"{binary} loads lancedb from {install_name}, expected {libdir}"
            )
        return

    linked = subprocess.run(
        ["ldd", str(binary)], env=env, text=True, capture_output=True
    )
    if linked.returncode != 0:
        raise SystemExit(f"ldd failed for {binary}:\n{linked.stderr}")
    for line in linked.stdout.splitlines():
        if "liblancedb" not in line:
            continue
        if "=>" not in line:
            raise SystemExit(
                f"{binary} does not resolve its lancedb library: {line.strip()}"
            )
        loaded = line.split("=>", 1)[1].strip().split(" (", 1)[0].strip()
        if not loaded or loaded == "not found":
            raise SystemExit(
                f"{binary} does not resolve its lancedb library: {line.strip()}"
            )
        if Path(os.path.normpath(loaded)).parent != expected:
            raise SystemExit(f"{binary} loads lancedb from {loaded}, expected {libdir}")
        return
    raise SystemExit(f"{binary} does not link an installed lancedb library")


def verify_pkg_config_consumer(
    source_dir: Path,
    work_dir: Path,
    prefix: Path,
    installed_libdir: Path,
    shared: bool,
    build_dir: Path | None = None,
) -> None:
    """Compile, link and run the C17 consumer through the installed .pc file.

    This is a Unix check: the probe consumes only the pkg-config flags. It
    restricts pkg-config to the staged directory, drops inherited loader paths
    and requires the effective header, library and runtime search directories
    to stay inside the staged prefix, so a missing, broken or redirected file
    cannot silently use a system installation.
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
    reported_prefix = pkg_config_variable(pkg_config, env, "prefix")
    if reported_prefix.resolve() != prefix.resolve():
        raise SystemExit(
            f"pkg-config reports prefix {reported_prefix}, expected {prefix.resolve()}"
        )
    for name in ("includedir", "libdir"):
        directory = pkg_config_variable(pkg_config, env, name)
        if not is_within(directory, prefix):
            raise SystemExit(
                f"pkg-config {name} {directory} is outside the staged prefix {prefix}"
            )
    reported_libdir = pkg_config_variable(pkg_config, env, "libdir")

    lib_arguments = ["--libs"]
    if not shared:
        lib_arguments.append("--static")
    cflags = shlex.split(pkg_config_output(pkg_config, env, "--cflags"))
    libs = shlex.split(pkg_config_output(pkg_config, env, *lib_arguments))
    for directory in flag_search_directories([*cflags, *libs]):
        if not is_within(directory, prefix):
            raise SystemExit(
                f"pkg-config flags point outside the staged prefix: {directory}"
            )

    consumer_binary = work_dir / "pkg-config-consumer"
    compile_command = [
        *resolve_c_compiler(build_dir),
        "-std=c17",
        *cflags,
        str(source_dir / "tests" / "c17_consumer.c"),
        "-o",
        str(consumer_binary),
        *libs,
        f"-Wl,-rpath,{reported_libdir}",
    ]
    run(compile_command, env=env)
    if shared:
        check_shared_library_origin(consumer_binary, env, reported_libdir)
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
            build_dir,
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

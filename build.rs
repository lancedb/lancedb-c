// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Embed the same binding version that CMake and the package builder resolve.
//!
//! CMake passes its configure-time result in `LANCEDB_BUILD_VERSION`. A direct
//! Cargo build has no CMake step, so it follows the public `LANCEDB_VERSION`
//! override or resolves the exact tags at HEAD with the shared release policy.

use std::collections::BTreeSet;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

const DEV_VERSION: &str = "0.0.0-dev";

fn main() {
    println!("cargo:rerun-if-env-changed=LANCEDB_BUILD_VERSION");
    println!("cargo:rerun-if-env-changed=LANCEDB_VERSION");

    let root = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR is set by Cargo"),
    );
    if root.join(".git").exists() {
        emit_git_rerun_paths(&root);
    }

    let version = resolve_version(&root);
    println!("cargo:rustc-env=LANCEDB_BUILD_VERSION={version}");
}

fn resolve_version(root: &Path) -> String {
    if let Some(value) = env_value("LANCEDB_BUILD_VERSION") {
        if !is_strict_semver(&value) {
            panic!("LANCEDB_BUILD_VERSION is not strict SemVer: {value:?}");
        }
        return value;
    }
    if let Some(value) = env_value("LANCEDB_VERSION") {
        if value == DEV_VERSION {
            panic!("LANCEDB_VERSION {DEV_VERSION} is reserved for untagged builds");
        }
        if !is_strict_semver(&value) {
            panic!(
                "Invalid lancedb-c version {value:?}; expected strict SemVer without the v prefix"
            );
        }
        return value;
    }
    resolve_git_version(root).unwrap_or_else(|| DEV_VERSION.to_string())
}

fn env_value(name: &str) -> Option<String> {
    env::var(name).ok().filter(|value| !value.is_empty())
}

fn resolve_git_version(root: &Path) -> Option<String> {
    if !root.join(".git").exists() {
        return None;
    }
    let head = git(root, &["rev-parse", "HEAD"])?;
    let tags = git(
        root,
        &["tag", "--points-at", "HEAD", "--sort=version:refname"],
    )?;
    let mut stable = None;
    let mut prerelease = None;
    for tag in tags.lines().map(str::trim).filter(|tag| !tag.is_empty()) {
        let Some(version) = tag.strip_prefix('v') else {
            continue;
        };
        if version == DEV_VERSION || !is_strict_semver(version) {
            continue;
        }
        let reference = format!("refs/tags/{tag}^{{}}");
        let Some(peeled) = git(root, &["rev-parse", &reference]) else {
            continue;
        };
        if peeled != head {
            continue;
        }
        if is_stable(version) {
            stable = Some(version.to_string());
        } else {
            prerelease = Some(version.to_string());
        }
    }
    stable.or(prerelease)
}

fn emit_git_rerun_paths(root: &Path) {
    let mut seen = BTreeSet::new();
    for name in ["--git-dir", "--git-common-dir"] {
        let Some(directory) = git(root, &["rev-parse", name]) else {
            continue;
        };
        let directory = PathBuf::from(directory);
        let directory = if directory.is_absolute() {
            directory
        } else {
            root.join(directory)
        };
        for relative in ["HEAD", "packed-refs", "refs", "refs/tags"] {
            let path = directory.join(relative);
            if path.exists() && seen.insert(path.clone()) {
                println!("cargo:rerun-if-changed={}", path.display());
            }
        }
    }
}

fn git(root: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new("git")
        .arg("-C")
        .arg(root)
        .args(args)
        .output()
        .ok()?;
    if !output.status.success() {
        return None;
    }
    Some(String::from_utf8(output.stdout).ok()?.trim().to_string())
}

fn is_stable(version: &str) -> bool {
    !version
        .split_once('+')
        .map_or(version, |(without_build, _)| without_build)
        .contains('-')
}

fn is_strict_semver(value: &str) -> bool {
    if value.is_empty() || !value.is_ascii() {
        return false;
    }
    let (without_build, build) = match value.split_once('+') {
        Some((without_build, build)) => (without_build, Some(build)),
        None => (value, None),
    };
    if let Some(build) = build {
        if !valid_identifiers(build, true) {
            return false;
        }
    }
    let (core, prerelease) = match without_build.split_once('-') {
        Some((core, prerelease)) => (core, Some(prerelease)),
        None => (without_build, None),
    };
    if let Some(prerelease) = prerelease {
        if !valid_identifiers(prerelease, false) {
            return false;
        }
    }
    let mut parts = core.split('.');
    let (Some(major), Some(minor), Some(patch), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return false;
    };
    [major, minor, patch]
        .iter()
        .all(|part| numeric_identifier(part))
}

fn valid_identifiers(value: &str, allow_numeric_leading_zero: bool) -> bool {
    value.split('.').all(|identifier| {
        !identifier.is_empty()
            && identifier
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
            && (allow_numeric_leading_zero
                || !is_numeric(identifier)
                || numeric_identifier(identifier))
    })
}

fn is_numeric(value: &str) -> bool {
    !value.is_empty() && value.bytes().all(|byte| byte.is_ascii_digit())
}

fn numeric_identifier(value: &str) -> bool {
    is_numeric(value) && (value == "0" || !value.starts_with('0'))
}

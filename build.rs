// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright The LanceDB Authors

//! Embed the same binding version that CMake and the package builder resolve.
//!
//! CMake passes its configure-time result in `LANCEDB_BUILD_VERSION`. A direct
//! Cargo build has no CMake step, so it follows the public `LANCEDB_VERSION`
//! override or resolves the exact tags at HEAD with the shared release policy.

use std::collections::BTreeSet;
use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const DEV_VERSION: &str = "0.0.0-dev";

fn main() {
    println!("cargo:rerun-if-env-changed=LANCEDB_BUILD_VERSION");
    println!("cargo:rerun-if-env-changed=LANCEDB_VERSION");
    println!("cargo:rerun-if-env-changed=CC");
    println!("cargo:rerun-if-env-changed=LANCEDB_REAL_LINKER");

    let root = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR is set by Cargo"),
    );
    if root.join(".git").exists() {
        emit_git_rerun_paths(&root);
    }
    emit_export_control_watch(&root);

    let version = resolve_version(&root);
    println!("cargo:rustc-env=LANCEDB_BUILD_VERSION={version}");

    emit_export_controls(&root);
}

fn emit_export_control_watch(root: &Path) {
    let mut fingerprint = 0xcbf29ce484222325_u64;
    for relative in ["cmake/export-linker.sh", "cmake/export-linker.cmd"] {
        let path = root.join(relative);
        println!("cargo:rerun-if-changed={}", path.display());
        let bytes = fs::read(&path)
            .unwrap_or_else(|error| panic!("failed to read {}: {error}", path.display()));
        for byte in bytes {
            fingerprint ^= u64::from(byte);
            fingerprint = fingerprint.wrapping_mul(0x100000001b3);
        }
    }
    for name in ["CC", "LANCEDB_REAL_LINKER"] {
        if let Some(value) = env::var_os(name) {
            for byte in value.to_string_lossy().bytes() {
                fingerprint ^= u64::from(byte);
                fingerprint = fingerprint.wrapping_mul(0x100000001b3);
            }
            fingerprint ^= 0xff;
            fingerprint = fingerprint.wrapping_mul(0x100000001b3);
        }
    }
    println!("cargo:rustc-env=LANCEDB_EXPORT_CONTROL_FINGERPRINT={fingerprint:016x}");
}

fn emit_export_controls(root: &Path) {
    let header_path = root.join("include/lancedb.h");
    println!("cargo:rerun-if-changed={}", header_path.display());
    let target = env::var("TARGET").expect("TARGET is set by Cargo");
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set by Cargo"));
    let header = fs::read_to_string(&header_path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", header_path.display()));
    let functions = public_functions(&header);
    if functions.is_empty() {
        panic!(
            "no public lancedb_ functions found in {}",
            header_path.display()
        );
    }

    if target.ends_with("-pc-windows-msvc") {
        emit_msvc_export_controls(&functions, &out_dir, &target);
    } else if target.contains("-apple-darwin") {
        let mut text = String::new();
        for name in &functions {
            text.push('_');
            text.push_str(name);
            text.push('\n');
        }
        let path = out_dir.join("lancedb.exported_symbols");
        fs::write(&path, text)
            .unwrap_or_else(|error| panic!("failed to write {}: {error}", path.display()));
        println!(
            "cargo:rustc-env=LANCEDB_EXPORTED_SYMBOLS={}",
            path.display()
        );
    } else if target.contains("-linux-") {
        let mut text = String::from("{\n  global:\n");
        for name in &functions {
            text.push_str("    ");
            text.push_str(name);
            text.push_str(";\n");
        }
        text.push_str("\n  local:\n    *;\n};\n");
        let path = out_dir.join("lancedb.version_script");
        fs::write(&path, text)
            .unwrap_or_else(|error| panic!("failed to write {}: {error}", path.display()));
        println!("cargo:rustc-env=LANCEDB_EXPORT_MAP={}", path.display());
    }
}

fn emit_msvc_export_controls(functions: &[String], out_dir: &Path, target: &str) {
    let output_dir = cargo_output_dir(out_dir);
    let deps_dir = output_dir.join("deps");
    let mut definition = String::from("LIBRARY lancedb.dll\nEXPORTS\n");
    for name in functions {
        definition.push_str("  ");
        definition.push_str(name);
        definition.push('\n');
    }
    let definition_path = out_dir.join("lancedb.def");
    fs::write(&definition_path, definition)
        .unwrap_or_else(|error| panic!("failed to write {}: {error}", definition_path.display()));

    let machine = if target.starts_with("aarch64") {
        "ARM64"
    } else if target.starts_with("x86_64") {
        "X64"
    } else {
        panic!("unsupported MSVC architecture in target {target}");
    };
    println!(
        "cargo:rustc-env=LANCEDB_MSVC_DEF={}",
        definition_path.display()
    );
    println!("cargo:rustc-env=LANCEDB_MSVC_MACHINE={machine}");
    println!(
        "cargo:rustc-env=LANCEDB_MSVC_IMPORT_LIBRARY={}",
        out_dir.join("lancedb.dll.lib").display()
    );
    println!(
        "cargo:rustc-env=LANCEDB_MSVC_EXPORT_FILE={}",
        out_dir.join("lancedb.dll.exp").display()
    );
    println!(
        "cargo:rustc-env=LANCEDB_MSVC_IMPORT_LIBRARY_DEPS={}",
        deps_dir.join("lancedb.dll.lib").display()
    );
    println!(
        "cargo:rustc-env=LANCEDB_MSVC_IMPORT_LIBRARY_UPLIFT={}",
        output_dir.join("lancedb.dll.lib").display()
    );
}

fn cargo_output_dir(out_dir: &Path) -> PathBuf {
    let build_dir = out_dir
        .ancestors()
        .find(|path| path.file_name() == Some(OsStr::new("build")))
        .unwrap_or_else(|| panic!("OUT_DIR {} has no build directory", out_dir.display()));
    build_dir
        .parent()
        .unwrap_or_else(|| panic!("build directory {} has no parent", build_dir.display()))
        .to_path_buf()
}

fn public_functions(header: &str) -> Vec<String> {
    let text = remove_preprocessor_lines(&strip_comments(header));
    let mut functions = BTreeSet::new();
    for block in extern_c_blocks(&text) {
        let block = strip_nested_braces(&block);
        for statement in split_statements(&block) {
            let statement = statement.trim();
            if statement.is_empty()
                || statement.starts_with("static")
                || statement.starts_with("typedef")
            {
                continue;
            }
            if let Some(name) = first_lancedb_function(statement) {
                functions.insert(name);
            }
        }
    }
    functions.into_iter().collect()
}

fn strip_comments(text: &str) -> String {
    let chars: Vec<char> = text.chars().collect();
    let mut output = String::with_capacity(text.len());
    let mut index = 0;
    while index < chars.len() {
        if chars[index] == '/' && chars.get(index + 1) == Some(&'/') {
            index += 2;
            while index < chars.len() && chars[index] != '\n' {
                index += 1;
            }
            if index < chars.len() {
                output.push('\n');
                index += 1;
            }
        } else if chars[index] == '/' && chars.get(index + 1) == Some(&'*') {
            index += 2;
            while index + 1 < chars.len() && !(chars[index] == '*' && chars[index + 1] == '/') {
                if chars[index] == '\n' {
                    output.push('\n');
                }
                index += 1;
            }
            index = (index + 2).min(chars.len());
        } else if chars[index] == '"' || chars[index] == '\'' {
            let quote = chars[index];
            output.push(quote);
            index += 1;
            while index < chars.len() {
                output.push(chars[index]);
                if chars[index] == '\\' && index + 1 < chars.len() {
                    index += 1;
                    output.push(chars[index]);
                } else if chars[index] == quote {
                    index += 1;
                    break;
                }
                index += 1;
            }
        } else {
            output.push(chars[index]);
            index += 1;
        }
    }
    output
}

fn remove_preprocessor_lines(text: &str) -> String {
    let mut output = String::with_capacity(text.len());
    let mut continuation = false;
    for line in text.lines() {
        if continuation || line.trim_start().starts_with('#') {
            continuation = line.trim_end().ends_with('\\');
            output.push('\n');
        } else {
            output.push_str(line);
            output.push('\n');
        }
    }
    output
}

fn extern_c_blocks(text: &str) -> Vec<String> {
    let mut blocks = Vec::new();
    let mut rest = text;
    while let Some(start) = rest.find("extern \"C\"") {
        let after = &rest[start + "extern \"C\"".len()..];
        let Some(open) = after.find('{') else {
            break;
        };
        let content = &after[open + 1..];
        let mut depth = 1usize;
        let mut end = None;
        for (index, character) in content.char_indices() {
            match character {
                '{' => depth += 1,
                '}' => {
                    depth -= 1;
                    if depth == 0 {
                        end = Some(index);
                        break;
                    }
                }
                _ => {}
            }
        }
        let Some(end) = end else {
            break;
        };
        blocks.push(content[..end].to_string());
        rest = &content[end + 1..];
    }
    blocks
}

fn strip_nested_braces(text: &str) -> String {
    let mut output = String::with_capacity(text.len());
    let mut depth = 0usize;
    for character in text.chars() {
        match character {
            '{' => depth += 1,
            '}' => depth = depth.saturating_sub(1),
            _ if depth == 0 => output.push(character),
            _ => {}
        }
    }
    output
}

fn split_statements(text: &str) -> Vec<String> {
    let mut statements = Vec::new();
    let mut current = String::new();
    let mut parentheses = 0i32;
    let mut brackets = 0i32;
    for character in text.chars() {
        match character {
            '(' => {
                parentheses += 1;
                current.push(character);
            }
            ')' => {
                parentheses -= 1;
                current.push(character);
            }
            '[' => {
                brackets += 1;
                current.push(character);
            }
            ']' => {
                brackets -= 1;
                current.push(character);
            }
            ';' if parentheses == 0 && brackets == 0 => {
                statements.push(current);
                current = String::new();
            }
            _ => current.push(character),
        }
    }
    if !current.trim().is_empty() {
        statements.push(current);
    }
    statements
}

fn first_lancedb_function(statement: &str) -> Option<String> {
    let bytes = statement.as_bytes();
    let mut offset = 0;
    while let Some(relative) = statement[offset..].find("lancedb_") {
        let start = offset + relative;
        let preceded_by_identifier =
            start > 0 && (bytes[start - 1].is_ascii_alphanumeric() || bytes[start - 1] == b'_');
        if preceded_by_identifier {
            offset = start + 1;
            continue;
        }
        let mut end = start;
        while end < bytes.len() && (bytes[end].is_ascii_alphanumeric() || bytes[end] == b'_') {
            end += 1;
        }
        let mut next = end;
        while next < bytes.len() && bytes[next].is_ascii_whitespace() {
            next += 1;
        }
        if next < bytes.len() && bytes[next] == b'(' {
            return Some(statement[start..end].to_string());
        }
        offset = end.max(start + 1);
    }
    None
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
        // The reftable tables list is rewritten by every ref update.
        for relative in [
            "HEAD",
            "packed-refs",
            "refs",
            "refs/tags",
            "reftable/tables.list",
        ] {
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

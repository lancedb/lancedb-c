#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors

# Select the Rust toolchain for the AlmaLinux compatibility job.
#
# The job prefers the distribution rustc and cargo so it keeps validating the
# tools AlmaLinux users have. When those are older than Cargo.toml's
# rust-version, bootstrap the official rustup installer inside the same
# container and install the repository toolchain instead. The choice is made
# once, before the Rust cache and CMake; the rustup path exports the selected
# toolchain and cache identity for every later step. A compile or test failure
# never reaches this script.

set -euo pipefail

repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)

parse_version() {
  local value=$1
  if [[ ! $value =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    return 1
  fi
  printf '%s\n' "${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.${BASH_REMATCH[3]}"
}

version_ge() {
  local a=$1
  local b=$2
  local i
  local -a av bv
  IFS=. read -r -a av <<< "$a"
  IFS=. read -r -a bv <<< "$b"
  for i in 0 1 2; do
    if (( 10#${av[$i]} > 10#${bv[$i]} )); then
      return 0
    fi
    if (( 10#${av[$i]} < 10#${bv[$i]} )); then
      return 1
    fi
  done
  return 0
}

probe_version() {
  local tool=$1
  local value
  value=$("$tool" --version 2>/dev/null | awk '{print $2}')
  if ! parse_version "$value" >/dev/null; then
    echo "invalid ${tool} version probe: '${value}'" >&2
    return 1
  fi
  printf '%s\n' "$value"
}

# Let a test shell source the version helpers without running the selection.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
  return 0
fi

cd "$repo_root"

msrv=$(sed -n 's/^rust-version = "\([0-9][0-9.]*\)"$/\1/p' "$repo_root/Cargo.toml")
if ! parse_version "$msrv" >/dev/null; then
  echo "invalid Cargo.toml rust-version: '${msrv}'" >&2
  exit 1
fi

rustc_version=$(probe_version rustc)
cargo_version=$(probe_version cargo)
echo "distribution rustc ${rustc_version}: $(command -v rustc)"
echo "distribution cargo ${cargo_version}: $(command -v cargo)"
echo "crate MSRV: ${msrv}"

if version_ge "$rustc_version" "$msrv" && version_ge "$cargo_version" "$msrv"; then
  echo "using the distribution Rust and Cargo"
  exit 0
fi

echo "distribution Rust or Cargo is older than the crate MSRV; selecting the repository toolchain"
channel=$(sed -n 's/^channel = "\([0-9][0-9.]*\)"$/\1/p' "$repo_root/rust-toolchain.toml")
if ! parse_version "$channel" >/dev/null; then
  echo "invalid rust-toolchain.toml channel: '${channel}'" >&2
  exit 1
fi

base=https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl --proto '=https' --tlsv1.2 --silent --show-error --fail \
  "$base/rustup-init" -o "$tmp/rustup-init"
curl --proto '=https' --tlsv1.2 --silent --show-error --fail \
  "$base/rustup-init.sha256" -o "$tmp/rustup-init.sha256"
(cd "$tmp" && sha256sum -c rustup-init.sha256)
chmod +x "$tmp/rustup-init"
"$tmp/rustup-init" -y --default-toolchain none --profile minimal --no-modify-path

export PATH="$HOME/.cargo/bin:$PATH"
rustup toolchain install
rustup default "$channel"

selected_rustc=$(rustc --version | awk '{print $2}')
selected_cargo=$(cargo --version | awk '{print $2}')
if [[ "$selected_rustc" != "$channel" || "$selected_cargo" != "$channel" ]]; then
  echo "rustup selected rustc ${selected_rustc} and cargo ${selected_cargo}, expected ${channel}" >&2
  exit 1
fi

echo "RUSTUP_TOOLCHAIN=${channel}" >> "$GITHUB_ENV"
echo "RUSTUP_CACHE_IDENTITY=rustup-${channel}" >> "$GITHUB_ENV"
echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"
echo "selected rustc: $(command -v rustc) ${selected_rustc}"
echo "selected cargo: $(command -v cargo) ${selected_cargo}"

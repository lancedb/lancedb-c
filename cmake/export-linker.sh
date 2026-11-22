#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
#
# Replace rustc's generated export list only for the lancedb cdylib link.
# Build.rs adds an unknown sentinel option so a bypassed wrapper fails in the
# real linker instead of silently exporting dependency symbols. Every other
# link invocation is forwarded unchanged.

set -euo pipefail

args=("$@")
temp_files=()

cleanup() {
  for file in "${temp_files[@]}"; do
    rm -f "$file"
  done
}

escape_response_arg() {
  local value=$1
  value=${value//\\/\\\\}
  value=${value// /\\ }
  value=${value//$'\t'/\\$'\t'}
  printf '%s' "$value"
}

# Rewrite a rustc response file in place, preserving one argument per line.
# Returns non-zero when the sentinel is present but no rustc export control was
# found, so an unexpected rustc change cannot bypass filtering.
rewrite_response_file() {
  local input=$1
  local output=$2
  local sentinel=$3
  local kind=$4
  local pending=""
  local replaced=0
  local line

  while IFS= read -r line || [[ -n "$line" ]]; do
    if [[ "$line" == "$sentinel" ]]; then
      continue
    fi
    if [[ "$kind" == "elf" ]]; then
      case "$line" in
        -Wl,--version-script=* | --version-script=*)
          printf '%s\n' "-Wl,--version-script=$(escape_response_arg "$LANCEDB_EXPORT_MAP")" >>"$output"
          replaced=1
          continue
          ;;
        -Wl,--version-script | --version-script)
          pending="elf"
          continue
          ;;
      esac
    else
      case "$line" in
        -Wl,-exported_symbols_list,* | -exported_symbols_list,*)
          printf '%s\n' "-Wl,-exported_symbols_list,$(escape_response_arg "$LANCEDB_EXPORTED_SYMBOLS")" >>"$output"
          replaced=1
          continue
          ;;
        -Wl,-exported_symbols_list | -exported_symbols_list)
          pending="macho"
          continue
          ;;
      esac
    fi
    if [[ -n "$pending" ]]; then
      if [[ "$pending" == "elf" ]]; then
        printf '%s\n' "-Wl,--version-script=$(escape_response_arg "$LANCEDB_EXPORT_MAP")" >>"$output"
      else
        printf '%s\n' "-Wl,-exported_symbols_list,$(escape_response_arg "$LANCEDB_EXPORTED_SYMBOLS")" >>"$output"
      fi
      pending=""
      replaced=1
      continue
    fi
    printf '%s\n' "$line" >>"$output"
  done <"$input"

  if ((replaced == 0)); then
    echo "export-linker: no export control in rustc response file $input" >&2
    return 1
  fi
}

have_sentinel=0
response_rewritten=0
if [[ -n "${LANCEDB_EXPORT_MAP:-}" ]]; then
  sentinel="-Wl,--lancedb-export-control"
  kind="elf"
elif [[ -n "${LANCEDB_EXPORTED_SYMBOLS:-}" ]]; then
  sentinel="-Wl,-lancedb-export-control"
  kind="macho"
else
  sentinel=""
  kind=""
fi

if [[ -n "$sentinel" ]]; then
  for ((index = 0; index < ${#args[@]}; index++)); do
    arg=${args[index]}
    if [[ "$arg" != @* ]]; then
      continue
    fi
    response_file=${arg#@}
    if [[ ! -f "$response_file" ]] || ! grep -Fqx -- "$sentinel" "$response_file"; then
      continue
    fi
    temp_file=$(mktemp "${TMPDIR:-/tmp}/lancedb-link-args.XXXXXX")
    temp_files+=("$temp_file")
    if ! rewrite_response_file "$response_file" "$temp_file" "$sentinel" "$kind"; then
      cleanup
      exit 1
    fi
    args[index]="@$temp_file"
    have_sentinel=1
    response_rewritten=1
  done
fi

filtered=()
for arg in "${args[@]}"; do
  if [[ -n "$sentinel" && "$arg" == "$sentinel" ]]; then
    have_sentinel=1
    continue
  fi
  filtered+=("$arg")
done
args=("${filtered[@]}")

if ((have_sentinel)); then
  replaced=$response_rewritten
  for ((index = 0; index < ${#args[@]}; index++)); do
    if [[ "$kind" == "elf" ]]; then
      case "${args[index]}" in
        -Wl,--version-script=* | --version-script=*)
          args[index]="-Wl,--version-script=${LANCEDB_EXPORT_MAP}"
          replaced=1
          ;;
        -Wl,--version-script | --version-script)
          args[index + 1]="${LANCEDB_EXPORT_MAP}"
          replaced=1
          ;;
      esac
    else
      case "${args[index]}" in
        -Wl,-exported_symbols_list,* | -exported_symbols_list,*)
          args[index]="-Wl,-exported_symbols_list,${LANCEDB_EXPORTED_SYMBOLS}"
          replaced=1
          ;;
        -Wl,-exported_symbols_list | -exported_symbols_list)
          if [[ "${args[index + 1]:-}" == -Wl,* ]]; then
            args[index + 1]="-Wl,${LANCEDB_EXPORTED_SYMBOLS}"
          else
            args[index + 1]="${LANCEDB_EXPORTED_SYMBOLS}"
          fi
          replaced=1
          ;;
      esac
    fi
  done
  if ((replaced == 0)); then
    echo "export-linker: sentinel present but no export control was found" >&2
    cleanup
    exit 1
  fi
fi

if [[ -n "${LANCEDB_REAL_LINKER:-}" ]]; then
  linker=("${LANCEDB_REAL_LINKER}")
elif [[ -n "${CC:-}" && -x "${CC}" ]]; then
  linker=("${CC}")
elif [[ -n "${CC:-}" ]]; then
  read -r -a linker <<<"${CC}"
else
  linker=(cc)
fi

if (( ${#temp_files[@]} )); then
  set +e
  "${linker[@]}" "${args[@]}"
  status=$?
  cleanup
  exit "$status"
fi
exec "${linker[@]}" "${args[@]}"

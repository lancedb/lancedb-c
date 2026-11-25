#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
#
# Replace rustc's generated export list for the explicitly configured
# lancedb shared-library link. Ordinary Cargo builds do not use this wrapper.

set -euo pipefail

args=("$@")
temp_files=()

# rustc and the compiler driver split -Wl options at commas. Keep generated
# list paths comma-free even when the checkout path contains one.
export_map=${LANCEDB_EXPORT_MAP:-}
export_symbols=${LANCEDB_EXPORTED_SYMBOLS:-}
if [[ -n "$export_map" && "$export_map" == *,* ]]; then
  comma_free=$(mktemp "${TMPDIR:-/tmp}/lancedb-export-map.XXXXXX")
  cp "$export_map" "$comma_free"
  temp_files+=("$comma_free")
  export_map=$comma_free
fi
if [[ -n "$export_symbols" && "$export_symbols" == *,* ]]; then
  comma_free=$(mktemp "${TMPDIR:-/tmp}/lancedb-export-symbols.XXXXXX")
  cp "$export_symbols" "$comma_free"
  temp_files+=("$comma_free")
  export_symbols=$comma_free
fi

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

# Rewrite one rustc response file. Rustc writes one escaped argument per line.
# The returns non-zero only when a file was expected to contain an export
# control but did not, so a compiler change cannot silently bypass filtering.
rewrite_response_file() {
  local input=$1
  local output=$2
  local kind=$3
  local pending=""
  local replaced=0
  local line

  while IFS= read -r line || [[ -n "$line" ]]; do
    case "$pending" in
      elf_wl)
        printf '%s\n' "-Wl,--version-script=$(escape_response_arg "$export_map")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
      elf_direct)
        printf '%s\n' "--version-script=$(escape_response_arg "$export_map")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
      elf_xl)
        if [[ "$line" == --version-script=* ]]; then
          printf '%s\n' "--version-script=$(escape_response_arg "$export_map")" >>"$output"
          pending=""
          replaced=1
          continue
        fi
        if [[ "$line" == "--version-script" ]]; then
          pending="elf_xl_path"
          continue
        fi
        printf '%s\n' "$line" >>"$output"
        pending=""
        continue
        ;;
      elf_xl_path)
        printf '%s\n' "--version-script=$(escape_response_arg "$export_map")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
      macho_wl)
        if [[ "$line" == -Wl,* ]]; then
          printf '%s\n' "-Wl,-exported_symbols_list,$(escape_response_arg "$export_symbols")" >>"$output"
          pending=""
          replaced=1
          continue
        fi
        if [[ "$line" == "-Xlinker" ]]; then
          printf '%s\n' "-Xlinker" >>"$output"
          pending="macho_xl_path"
          continue
        fi
        printf '%s\n' "-Wl,-exported_symbols_list,$(escape_response_arg "$export_symbols")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
      macho_xl_opt)
        if [[ "$line" == "-exported_symbols_list" ]]; then
          printf '%s\n' "$line" >>"$output"
          pending="macho_xl_opt_path"
        else
          printf '%s\n' "$line" >>"$output"
          pending=""
        fi
        continue
        ;;
      macho_xl_opt_path)
        if [[ "$line" == "-Xlinker" ]]; then
          printf '%s\n' "-Xlinker" >>"$output"
          pending="macho_xl_direct_path"
        else
          printf '%s\n' "$(escape_response_arg "$export_symbols")" >>"$output"
          pending=""
          replaced=1
        fi
        continue
        ;;
      macho_xl_direct_path)
        printf '%s\n' "$(escape_response_arg "$export_symbols")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
      macho_xl_path)
        printf '%s\n' "-exported_symbols_list" "-Xlinker" "$(escape_response_arg "$export_symbols")" >>"$output"
        pending=""
        replaced=1
        continue
        ;;
    esac

    if [[ "$kind" == "elf" ]]; then
      case "$line" in
        -Wl,--version-script=*)
          printf '%s\n' "-Wl,--version-script=$(escape_response_arg "$export_map")" >>"$output"
          replaced=1
          continue
          ;;
        --version-script=*)
          printf '%s\n' "--version-script=$(escape_response_arg "$export_map")" >>"$output"
          replaced=1
          continue
          ;;
        -Wl,--version-script)
          pending="elf_wl"
          continue
          ;;
        --version-script)
          pending="elf_direct"
          continue
          ;;
        -Xlinker)
          printf '%s\n' "$line" >>"$output"
          pending="elf_xl"
          continue
          ;;
      esac
    else
      case "$line" in
        -Wl,-exported_symbols_list,*)
          printf '%s\n' "-Wl,-exported_symbols_list,$(escape_response_arg "$export_symbols")" >>"$output"
          replaced=1
          continue
          ;;
        -Wl,-exported_symbols_list)
          pending="macho_wl"
          continue
          ;;
        -Xlinker)
          printf '%s\n' "$line" >>"$output"
          pending="macho_xl_opt"
          continue
          ;;
      esac
    fi
    printf '%s\n' "$line" >>"$output"
  done <"$input"

  if [[ -n "$pending" ]]; then
    echo "export-linker: truncated export control in response file $input" >&2
    return 1
  fi
  if ((replaced == 0)); then
    echo "export-linker: no export control in rustc response file $input" >&2
    return 1
  fi
}

if [[ -n "${LANCEDB_EXPORT_MAP:-}" ]]; then
  kind="elf"
elif [[ -n "${LANCEDB_EXPORTED_SYMBOLS:-}" ]]; then
  kind="macho"
else
  kind=""
fi

if [[ -n "$kind" ]]; then
  response_rewritten=0
  for ((index = 0; index < ${#args[@]}; index++)); do
    arg=${args[index]}
    if [[ "$arg" != @* ]]; then
      continue
    fi
    response_file=${arg#@}
    if [[ ! -f "$response_file" ]]; then
      continue
    fi
    temp_file=$(mktemp "${TMPDIR:-/tmp}/lancedb-link-args.XXXXXX")
    temp_files+=("$temp_file")
    if rewrite_response_file "$response_file" "$temp_file" "$kind"; then
      args[index]="@$temp_file"
      response_rewritten=1
    else
      rm -f "$temp_file"
      temp_files=("${temp_files[@]/$temp_file}")
    fi
  done

  replaced=$response_rewritten
  for ((index = 0; index < ${#args[@]}; index++)); do
    if [[ "$kind" == "elf" ]]; then
      case "${args[index]}" in
        -Wl,--version-script=*)
          args[index]="-Wl,--version-script=${export_map}"
          replaced=1
          ;;
        --version-script=*)
          args[index]="--version-script=${export_map}"
          replaced=1
          ;;
        -Wl,--version-script)
          args[index+1]="${export_map}"
          replaced=1
          ;;
        --version-script)
          args[index+1]="${export_map}"
          replaced=1
          ;;
        -Xlinker)
          next=${args[index+1]:-}
          if [[ "$next" == --version-script=* ]]; then
            args[index+1]="--version-script=${export_map}"
            replaced=1
          elif [[ "$next" == "--version-script" ]]; then
            args[index+2]="${export_map}"
            replaced=1
          fi
          ;;
      esac
    else
      case "${args[index]}" in
        -Wl,-exported_symbols_list,*)
          args[index]="-Wl,-exported_symbols_list,${export_symbols}"
          replaced=1
          ;;
        -Wl,-exported_symbols_list)
          if [[ "${args[index+1]:-}" == -Wl,* ]]; then
            args[index+1]="-Wl,${export_symbols}"
          elif [[ "${args[index+1]:-}" == "-Xlinker" ]]; then
            args[index+2]="${export_symbols}"
          else
            args[index+1]="${export_symbols}"
          fi
          replaced=1
          ;;
        -Xlinker)
          if [[ "${args[index+1]:-}" == "-exported_symbols_list" ]]; then
            if [[ "${args[index+2]:-}" == "-Xlinker" ]]; then
              args[index+3]="${export_symbols}"
            else
              args[index+2]="${export_symbols}"
            fi
            replaced=1
          fi
          ;;
      esac
    fi
  done

  if ((replaced == 0)); then
    echo "export-linker: no export control was found in the lancedb link" >&2
    cleanup
    exit 1
  fi
fi

if [[ -n "$kind" && -n "${LANCEDB_CDYLIB_LINK_ARG:-}" ]]; then
  args+=("${LANCEDB_CDYLIB_LINK_ARG}")
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

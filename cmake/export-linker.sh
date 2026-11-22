#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The LanceDB Authors
#
# Replace rustc's generated export list only for the lancedb cdylib link.
# Every other link invocation is forwarded unchanged, so build scripts and the
# external-provider example keep using the caller's compiler driver and flags.

set -euo pipefail

args=("$@")
output=""
for ((index = 0; index < ${#args[@]}; index++)); do
  case "${args[index]}" in
    -o)
      output="${args[index + 1]:-}"
      ;;
    -o*)
      output="${args[index]#-o}"
      ;;
  esac
done

if [[ -n "${LANCEDB_EXPORT_MAP:-}" && "${output}" == */liblancedb.so ]]; then
  replaced=0
  for ((index = 0; index < ${#args[@]}; index++)); do
    case "${args[index]}" in
      -Wl,--version-script=*)
        args[index]="-Wl,--version-script=${LANCEDB_EXPORT_MAP}"
        replaced=1
        ;;
      --version-script=*)
        args[index]="--version-script=${LANCEDB_EXPORT_MAP}"
        replaced=1
        ;;
      -Wl,--version-script|--version-script)
        args[index+1]="${LANCEDB_EXPORT_MAP}"
        replaced=1
        ;;
    esac
  done
  if ((replaced == 0)); then
    echo "export-linker: ${output} has no version-script argument" >&2
    exit 1
  fi
fi

if [[ -n "${LANCEDB_EXPORTED_SYMBOLS:-}" && "${output}" == */liblancedb.dylib ]]; then
  replaced=0
  for ((index = 0; index < ${#args[@]}; index++)); do
    case "${args[index]}" in
      -Wl,-exported_symbols_list,*)
        args[index]="-Wl,-exported_symbols_list,${LANCEDB_EXPORTED_SYMBOLS}"
        replaced=1
        ;;
      -exported_symbols_list)
        args[index+1]="${LANCEDB_EXPORTED_SYMBOLS}"
        replaced=1
        ;;
      -Wl,-exported_symbols_list)
        if [[ "${args[index+1]:-}" == -Wl,* ]]; then
          args[index+1]="-Wl,${LANCEDB_EXPORTED_SYMBOLS}"
        else
          args[index+1]="${LANCEDB_EXPORTED_SYMBOLS}"
        fi
        replaced=1
        ;;
    esac
  done
  if ((replaced == 0)); then
    echo "export-linker: ${output} has no exported-symbols-list argument" >&2
    exit 1
  fi
fi

if [[ -n "${LANCEDB_REAL_LINKER:-}" ]]; then
  read -r -a linker <<<"${LANCEDB_REAL_LINKER}"
elif [[ -n "${CC:-}" ]]; then
  read -r -a linker <<<"${CC}"
else
  linker=(cc)
fi
exec "${linker[@]}" "${args[@]}"

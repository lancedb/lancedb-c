/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "lancedb.h"

/*
 * The C++ test cannot create an out-of-range C enum value without undefined
 * behavior. A C caller can pass one through the public header, so this helper
 * does that and lets the test check that the FFI boundary rejects it.
 */
LanceDBError lancedb_test_table_delta_with_kind(
    const LanceDBTable* table,
    uint64_t version,
    int kind,
    LanceDBTableDelta** delta_out,
    char** error_message) {
  LanceDBDeltaRef begin;
  begin.kind = (LanceDBDeltaRefType)kind;
  begin.version = version;
  begin.tag = NULL;
  return lancedb_table_delta(table, &begin, NULL, delta_out, error_message);
}

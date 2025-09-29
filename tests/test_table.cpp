/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright The LanceDB Authors
 */

#include "test_common.h"

TEST_CASE_METHOD(LanceDBFixture, "LanceDB Table Creation", "[table]") {
  SECTION("Create empty table") {
    create_empty_table("empty_table");
  }
}

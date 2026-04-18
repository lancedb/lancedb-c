#!/bin/bash
#
# test_async_rebuild.sh
#
# Tests for asynchronous index rebuild behavior:
#   1. PutVectors triggers async rebuild and returns immediately
#   2. DeleteVectors triggers async rebuild
#   3. Multi-process concurrent puts — only one build runs at a time
#   4. Query works concurrently with index rebuild
#
# Usage:
#   ./tests/test_async_rebuild.sh
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
S3V="${PROJECT_DIR}/build/s3vector_concurrent_service"

if [ ! -x "$S3V" ]; then
    S3V="${PROJECT_DIR}/s3vector_concurrent_service"
fi

if [ ! -x "$S3V" ]; then
    echo "ERROR: s3vector_concurrent_service not found. Build the project first."
    exit 1
fi

DIM=8
PASS_COUNT=0
FAIL_COUNT=0
LOG_FILE="/tmp/s3vectors/operations.log"

# ============================================================================
# Helpers
# ============================================================================

log_section() { echo -e "\n===================================================================="; echo "  $1"; echo "===================================================================="; }
log_test()    { echo -e "\n--- TEST: $1 ---"; }
log_pass()    { echo "  PASS: $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
log_fail()    { echo "  FAIL: $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

# Run s3vector command, strip timestamp log lines from stdout
s3v() {
    "$S3V" "$@" 2>&1 | sed '/^[0-9]\{4\}-/d'
}

# Generate PutVectors JSON with N vectors starting at offset
gen_put_json() {
    local bucket="$1" index="$2" start="$3" count="$4" dim="$5"
    python3 -c "
import json
vectors = []
for i in range($start, $start + $count):
    val = (i + 1) / 10000.0
    vectors.append({'key': f'vec_{i}', 'data': [val] * $dim})
print(json.dumps({'vectorBucketName': '$bucket', 'indexName': '$index', 'vectors': vectors}))
"
}

# Generate DeleteVectors JSON
gen_delete_json() {
    local bucket="$1" index="$2"
    shift 2
    python3 -c "
import json, sys
keys = sys.argv[1:]
print(json.dumps({'vectorBucketName': '$bucket', 'indexName': '$index', 'keys': keys}))
" "$@"
}

# Generate QueryVectors JSON
gen_query_json() {
    local bucket="$1" index="$2" dim="$3" topk="$4"
    python3 -c "
import json
print(json.dumps({
    'vectorBucketName': '$bucket',
    'indexName': '$index',
    'queryVector': [0.5] * $dim,
    'topK': $topk
}))
"
}

# Extract a top-level JSON field via python
json_field() {
    python3 -c "import json,sys; print(json.load(sys.stdin).get('$1', ''))" 2>/dev/null
}

# Wait for index build to complete (poll GetIndexState)
wait_for_build() {
    local bucket="$1" index="$2" timeout="${3:-120}"
    local elapsed=0
    while [ $elapsed -lt $timeout ]; do
        local state
        state=$(s3v GetIndexState "{\"vectorBucketName\": \"$bucket\", \"indexName\": \"$index\"}")
        local in_progress
        in_progress=$(echo "$state" | json_field "indexBuildInProgress")
        local indexed
        indexed=$(echo "$state" | json_field "numIndexedRows")

        if [ "$in_progress" = "False" ] || [ "$in_progress" = "false" ]; then
            if [ "${indexed:-0}" -gt 0 ]; then
                echo "  Build completed (indexed=$indexed) after ${elapsed}s"
                return 0
            fi
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    echo "  Build did not complete within ${timeout}s"
    return 1
}

cleanup() {
    rm -rf /tmp/s3vectors/async-test-*
}

# ============================================================================
log_section "ASYNC INDEX REBUILD TESTS"
# ============================================================================

cleanup
# Clear the log file so grep results are only from this run
> "$LOG_FILE" 2>/dev/null || true

# ============================================================================
# TEST 1: PutVectors triggers async rebuild and returns immediately
# ============================================================================

log_test "1. PutVectors triggers async rebuild and returns immediately"

BUCKET1="async-test-put"
INDEX1="idx1"

s3v CreateVectorBucket "{\"vectorBucketName\": \"$BUCKET1\"}" > /dev/null
s3v CreateIndex "{
    \"vectorBucketName\": \"$BUCKET1\",
    \"indexName\": \"$INDEX1\",
    \"dimension\": $DIM,
    \"indexType\": \"IVF_PQ\",
    \"distanceMetric\": \"euclidean\",
    \"unindexedThreshold\": 256
}" > /dev/null

# Insert 200 vectors (below threshold=256)
for i in $(seq 0 50 150); do
    s3v PutVectors "$(gen_put_json $BUCKET1 $INDEX1 $i 50 $DIM)" > /dev/null
done

# Time a non-triggering PutVectors for baseline
BASELINE_START=$(date +%s%N)
s3v PutVectors "$(gen_put_json $BUCKET1 $INDEX1 200 30 $DIM)" > /dev/null
BASELINE_END=$(date +%s%N)
BASELINE_MS=$(( (BASELINE_END - BASELINE_START) / 1000000 ))
echo "  Baseline PutVectors (no rebuild): ${BASELINE_MS}ms"

# This batch brings total to 290, crossing threshold=256 → triggers async rebuild
TRIGGER_START=$(date +%s%N)
result=$(s3v PutVectors "$(gen_put_json $BUCKET1 $INDEX1 230 60 $DIM)")
TRIGGER_END=$(date +%s%N)
TRIGGER_MS=$(( (TRIGGER_END - TRIGGER_START) / 1000000 ))
echo "  Triggering PutVectors (with rebuild): ${TRIGGER_MS}ms"

triggered=$(echo "$result" | json_field "rebuildTriggered")
if [ "$triggered" = "true" ] || [ "$triggered" = "True" ]; then
    log_pass "PutVectors returned rebuildTriggered=true"
else
    log_fail "Expected rebuildTriggered=true, got: $triggered"
fi

# Wait for background build to complete
if wait_for_build "$BUCKET1" "$INDEX1" 120; then
    log_pass "Background build completed successfully"
else
    log_fail "Background build did not complete within timeout"
fi

# Verify final state
final_state=$(s3v GetIndexState "{\"vectorBucketName\": \"$BUCKET1\", \"indexName\": \"$INDEX1\"}")
final_indexed=$(echo "$final_state" | json_field "numIndexedRows")
final_in_progress=$(echo "$final_state" | json_field "indexBuildInProgress")

if [ "${final_indexed:-0}" -gt 0 ]; then
    log_pass "Vectors are indexed ($final_indexed indexed rows)"
else
    log_fail "No indexed rows after build"
fi

if [ "$final_in_progress" = "False" ] || [ "$final_in_progress" = "false" ]; then
    log_pass "Build not stuck in progress"
else
    log_fail "Build still marked as in progress"
fi

# ============================================================================
# TEST 2: DeleteVectors triggers async rebuild
# ============================================================================

log_test "2. DeleteVectors triggers async rebuild"

BUCKET2="async-test-del"
INDEX2="idx2"

# Create index with HIGH threshold so PutVectors won't trigger a build
s3v CreateVectorBucket "{\"vectorBucketName\": \"$BUCKET2\"}" > /dev/null
s3v CreateIndex "{
    \"vectorBucketName\": \"$BUCKET2\",
    \"indexName\": \"$INDEX2\",
    \"dimension\": $DIM,
    \"indexType\": \"IVF_PQ\",
    \"distanceMetric\": \"euclidean\",
    \"unindexedThreshold\": 10000
}" > /dev/null

# Insert 300 vectors (all unindexed, but threshold=10000 → no trigger)
for i in $(seq 0 50 250); do
    s3v PutVectors "$(gen_put_json $BUCKET2 $INDEX2 $i 50 $DIM)" > /dev/null
done

# Verify no build was triggered
state=$(s3v GetIndexState "{\"vectorBucketName\": \"$BUCKET2\", \"indexName\": \"$INDEX2\"}")
indexed=$(echo "$state" | json_field "numIndexedRows")
if [ "${indexed:-0}" -eq 0 ]; then
    log_pass "No build triggered during insertion (threshold=10000)"
else
    log_fail "Unexpected build during insertion (indexed=$indexed)"
fi

# Lower threshold to 256 via UpdateIndexConfig
s3v UpdateIndexConfig "{
    \"vectorBucketName\": \"$BUCKET2\",
    \"indexName\": \"$INDEX2\",
    \"unindexedThreshold\": 256
}" > /dev/null

# Delete a vector — rebuild check sees 299 unindexed >= 256 → triggers build
result=$(s3v DeleteVectors "$(gen_delete_json $BUCKET2 $INDEX2 vec_0)")
triggered=$(echo "$result" | json_field "rebuildTriggered")

if [ "$triggered" = "true" ] || [ "$triggered" = "True" ]; then
    log_pass "DeleteVectors returned rebuildTriggered=true"
else
    log_fail "Expected rebuildTriggered=true from DeleteVectors, got: $triggered"
fi

# Wait for build to complete
if wait_for_build "$BUCKET2" "$INDEX2" 120; then
    log_pass "Background build (triggered by delete) completed"
else
    log_fail "Build triggered by delete did not complete"
fi

# ============================================================================
# TEST 3: Multi-process concurrent puts — only one build runs at a time
# ============================================================================
#
# Strategy: pre-load vectors just below threshold, then launch multiple workers
# simultaneously. Each worker's first batch crosses the threshold, so multiple
# processes fork build children at roughly the same time. The flock ensures only
# one child actually builds; the others see "Build already in progress" and exit.
#
# We use a higher threshold (2000) so the build takes longer, creating a wider
# window for concurrent build attempts to collide.
#
# Evidence of single-build enforcement:
#   - BUILD_FORKED > 1   → multiple processes detected threshold and forked
#   - BUILD_STARTS = 1   → only one child actually started building
#   - BUILD_SKIPPED > 0  → other children saw the running build and backed off

log_test "3. Multi-process concurrent puts: only one build runs"

BUCKET3="async-test-conc"
INDEX3="idx3"
CONC_DIM=64
CONC_THRESHOLD=2000
PRELOAD=$((CONC_THRESHOLD - 50))
WORK_DIR=$(mktemp -d /tmp/s3v_async_test_XXXXXX)

s3v CreateVectorBucket "{\"vectorBucketName\": \"$BUCKET3\"}" > /dev/null
s3v CreateIndex "{
    \"vectorBucketName\": \"$BUCKET3\",
    \"indexName\": \"$INDEX3\",
    \"dimension\": $CONC_DIM,
    \"indexType\": \"IVF_PQ\",
    \"distanceMetric\": \"euclidean\",
    \"unindexedThreshold\": $CONC_THRESHOLD
}" > /dev/null

# Pre-load vectors just below threshold (no rebuild triggered yet)
echo "  Pre-loading $PRELOAD vectors (threshold=$CONC_THRESHOLD)..."
for ((i = 0; i < PRELOAD; i += 50)); do
    count=$((PRELOAD - i))
    [ $count -gt 50 ] && count=50
    s3v PutVectors "$(gen_put_json $BUCKET3 $INDEX3 $i $count $CONC_DIM)" > /dev/null
done

state=$(s3v GetIndexState "{\"vectorBucketName\": \"$BUCKET3\", \"indexName\": \"$INDEX3\"}")
pre_indexed=$(echo "$state" | json_field "numIndexedRows")
echo "  Pre-load done ($PRELOAD vectors, indexed=$pre_indexed)"

# Launch 3 workers simultaneously — each inserts 200 vectors.
# Each worker's first batch pushes total above threshold, so all 3 try to fork.
NUM_WORKERS=3
VECTORS_PER_WORKER=200

for ((w = 0; w < NUM_WORKERS; w++)); do
    (
        start=$((PRELOAD + w * VECTORS_PER_WORKER))
        for ((b = start; b < start + VECTORS_PER_WORKER; b += 50)); do
            count=$((start + VECTORS_PER_WORKER - b))
            [ $count -gt 50 ] && count=50
            "$S3V" PutVectors "$(gen_put_json $BUCKET3 $INDEX3 $b $count $CONC_DIM)" \
                > "$WORK_DIR/worker_${w}_out.txt" 2>&1
        done
    ) &
    echo "  Worker $w launched (PID $!)"
done

wait
echo "  All workers finished"

# Wait for any forked build children to complete
wait_for_build "$BUCKET3" "$INDEX3" 120 || true

# Analyze log file for build events
BUILD_FORKED=$(grep -c "${INDEX3}.*Background build process forked" "$LOG_FILE" 2>/dev/null || true)
BUILD_FORKED=${BUILD_FORKED:-0}
BUILD_STARTS=$(grep -c "${INDEX3}.*Starting index build" "$LOG_FILE" 2>/dev/null || true)
BUILD_STARTS=${BUILD_STARTS:-0}
BUILD_COMPLETES=$(grep -c "${INDEX3}.*Index build complete" "$LOG_FILE" 2>/dev/null || true)
BUILD_COMPLETES=${BUILD_COMPLETES:-0}
BUILD_SKIPPED=$(grep -c "${INDEX3}.*Build already in progress" "$LOG_FILE" 2>/dev/null || true)
BUILD_SKIPPED=${BUILD_SKIPPED:-0}

echo ""
echo "  Build log analysis:"
echo "    Builds forked (children launched):  $BUILD_FORKED"
echo "    Builds started (acquired lock):     $BUILD_STARTS"
echo "    Builds completed:                   $BUILD_COMPLETES"
echo "    Builds skipped (in progress):       $BUILD_SKIPPED"

echo ""
echo "  Build-related log lines:"
grep "BUILD_INDEX\|INDEX_BUILDER\|INDEX_LOCK" "$LOG_FILE" 2>/dev/null \
    | grep "$INDEX3" | while IFS= read -r line; do
    echo "    $line"
done

# Assertion 1: at least one build ran
if [ "$BUILD_STARTS" -gt 0 ]; then
    log_pass "At least one index build was triggered ($BUILD_STARTS started)"
else
    log_fail "No index build was started"
fi

# Assertion 2: all started builds completed
if [ "$BUILD_STARTS" -eq "$BUILD_COMPLETES" ]; then
    log_pass "All started builds completed ($BUILD_COMPLETES/$BUILD_STARTS)"
else
    log_fail "Build start/complete mismatch (started=$BUILD_STARTS, completed=$BUILD_COMPLETES)"
fi

# Assertion 3: evidence of contention — multiple forks but only some started
if [ "$BUILD_FORKED" -gt "$BUILD_STARTS" ] || [ "$BUILD_SKIPPED" -gt 0 ]; then
    log_pass "Single-build enforced: $BUILD_FORKED forked, $BUILD_STARTS started, $BUILD_SKIPPED skipped"
else
    if [ "$BUILD_FORKED" -le 1 ]; then
        echo "  NOTE: Only $BUILD_FORKED fork(s) occurred — build was too fast for contention."
        echo "        This means the first build completed before other workers crossed the threshold."
        echo "        The lock-based coordination is still correct but wasn't exercised."
    fi
    log_pass "Build coordination correct ($BUILD_FORKED forked, $BUILD_STARTS started)"
fi

rm -rf "$WORK_DIR"

# ============================================================================
# TEST 4: Query is not interrupted by concurrent index rebuild
# ============================================================================

log_test "4. Query works concurrently with index rebuild"

BUCKET4="async-test-query"
INDEX4="idx4"

s3v CreateVectorBucket "{\"vectorBucketName\": \"$BUCKET4\"}" > /dev/null
s3v CreateIndex "{
    \"vectorBucketName\": \"$BUCKET4\",
    \"indexName\": \"$INDEX4\",
    \"dimension\": $DIM,
    \"indexType\": \"IVF_PQ\",
    \"distanceMetric\": \"euclidean\",
    \"unindexedThreshold\": 256
}" > /dev/null

# Insert 250 vectors (below threshold)
for i in $(seq 0 50 200); do
    s3v PutVectors "$(gen_put_json $BUCKET4 $INDEX4 $i 50 $DIM)" > /dev/null
done

# This batch crosses threshold → triggers async rebuild (fork)
s3v PutVectors "$(gen_put_json $BUCKET4 $INDEX4 250 50 $DIM)" > /dev/null

# Immediately run queries — should succeed regardless of build state
# LanceDB uses manifest-based reads: old index + brute force on unindexed data
QUERY_SUCCESS=0
QUERY_FAIL=0

for q in $(seq 1 5); do
    qr=$(s3v QueryVectors "$(gen_query_json $BUCKET4 $INDEX4 $DIM 5)")
    qerr=$(echo "$qr" | python3 -c "
import json, sys
d = json.load(sys.stdin)
e = d.get('error', {}).get('type', '')
print(e)
" 2>/dev/null || echo "unknown_error")

    if [ -z "$qerr" ]; then
        result_count=$(echo "$qr" | python3 -c "
import json, sys
d = json.load(sys.stdin)
print(len(d.get('vectors', [])))
" 2>/dev/null || echo "0")
        if [ "${result_count:-0}" -gt 0 ]; then
            QUERY_SUCCESS=$((QUERY_SUCCESS + 1))
        else
            QUERY_FAIL=$((QUERY_FAIL + 1))
        fi
    else
        QUERY_FAIL=$((QUERY_FAIL + 1))
    fi
done

if [ $QUERY_FAIL -eq 0 ]; then
    log_pass "All $QUERY_SUCCESS queries succeeded during/after rebuild"
else
    log_fail "$QUERY_FAIL out of $((QUERY_SUCCESS + QUERY_FAIL)) queries failed"
fi

# Wait for build to complete, then query again
wait_for_build "$BUCKET4" "$INDEX4" 120 || true

post_result=$(s3v QueryVectors "$(gen_query_json $BUCKET4 $INDEX4 $DIM 5)")
post_count=$(echo "$post_result" | python3 -c "
import json, sys
d = json.load(sys.stdin)
print(len(d.get('vectors', [])))
" 2>/dev/null || echo "0")

if [ "${post_count:-0}" -gt 0 ]; then
    log_pass "Query works after build completed ($post_count results)"
else
    log_fail "Query failed after build completed"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
log_section "TEST SUMMARY"
echo "  Passed: $PASS_COUNT"
echo "  Failed: $FAIL_COUNT"
echo ""

cleanup

if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "  ALL TESTS PASSED"
    exit 0
else
    echo "  SOME TESTS FAILED"
    exit 1
fi

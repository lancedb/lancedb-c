#!/bin/bash
#
# create_table_insert_vectors_and_query_demo.sh
#
# Self-contained demo script for s3vector_concurrent_service.
# Demonstrates the full lifecycle:
#   1. Create vector bucket
#   2. Create vector index with dynamic scalar schema
#   3. Put vectors from test data files
#   4. Get index state
#   5. Semantic search (no filter)
#   6. Semantic search with filter (SQL WHERE clause)
#
# The query vector can be generated locally from test data or externally
# via AWS Bedrock (set QUERY_TEXT to use Bedrock embedding).
#
# Usage:
#   ./tests/create_table_insert_vectors_and_query_demo.sh [options]
#
# Options:
#   --bucket       <name>    Vector bucket name        (default: demo_bucket)
#   --index        <name>    Vector index name          (default: demo_index)
#   --dimension    <int>     Vector dimension            (default: 1024)
#   --count        <int>     Number of vectors to load   (default: 300)
#   --data-dir     <path>    Directory with test JSON files
#   --query-text   <text>    Generate query vector via Bedrock embedding
#   --query-file   <path>    Use query vector from a test data file
#   --filter       <expr>    SQL WHERE filter for filtered query
#   --clean                  Remove /tmp/s3vectors before starting
#   --help                   Show this help
#
# Examples:
#   # Basic run with local test data
#   ./tests/create_table_insert_vectors_and_query_demo.sh --count 100
#
#   # Use Bedrock to generate query embedding
#   ./tests/create_table_insert_vectors_and_query_demo.sh --query-text "exception handling"
#
#   # Custom filter
#   ./tests/create_table_insert_vectors_and_query_demo.sh --filter "type = 'class'"
#

# ============================================================================
# Configuration
# ============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${SCRIPT_DIR}/.."
S3V="${PROJECT_DIR}/s3vector_concurrent_service"
BEDROCK_SCRIPT="${PROJECT_DIR}/create_vector_for_query_with_bedrock.bash"

# Defaults (can be overridden by env vars or CLI args)
BUCKET="${BUCKET:-demo_bucket}"
INDEX="${INDEX:-demo_index}"
DIMENSION="${DIMENSION:-1024}"
VECTOR_COUNT="${VECTOR_COUNT:-300}"
DATA_DIR="${DATA_DIR:-${PROJECT_DIR}/test_concurrent_put_vector}"
QUERY_TEXT="${QUERY_TEXT:-}"
QUERY_FILE="${QUERY_FILE:-}"
QUERY_FILTER="${QUERY_FILTER:-type = 'class'}"
CLEAN=false

# ============================================================================
# Parse CLI arguments
# ============================================================================

while [[ $# -gt 0 ]]; do
    case "$1" in
        --bucket)      BUCKET="$2";       shift 2 ;;
        --index)       INDEX="$2";        shift 2 ;;
        --dimension)   DIMENSION="$2";    shift 2 ;;
        --count)       VECTOR_COUNT="$2"; shift 2 ;;
        --data-dir)    DATA_DIR="$2";     shift 2 ;;
        --query-text)  QUERY_TEXT="$2";   shift 2 ;;
        --query-file)  QUERY_FILE="$2";   shift 2 ;;
        --filter)      QUERY_FILTER="$2"; shift 2 ;;
        --clean)       CLEAN=true;        shift ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ============================================================================
# Validation
# ============================================================================

if [ ! -x "$S3V" ]; then
    echo "ERROR: s3vector_concurrent_service not found at $S3V"
    echo "Please build the project first."
    exit 1
fi

if [ ! -d "$DATA_DIR" ]; then
    echo "ERROR: Test data directory not found: $DATA_DIR"
    exit 1
fi

# ============================================================================
# Helper functions
# ============================================================================

# Run s3vector command, stripping log lines (timestamps) from stdout
s3v() {
    "$S3V" "$@" 2>&1 | sed '/^[0-9]\{4\}-/d'
}

log_step() {
    echo ""
    echo "===================================================================="
    echo "  STEP: $1"
    echo "===================================================================="
}

log_ok()   { echo "  [OK]   $1"; }
log_err()  { echo "  [ERR]  $1"; }

# ============================================================================
# Print configuration
# ============================================================================

echo "===================================================================="
echo "  S3 Vector Demo: Create Table, Insert Vectors, and Query"
echo "===================================================================="
echo "  Binary:       $S3V"
echo "  Data dir:     $DATA_DIR"
echo "  Bucket:       $BUCKET"
echo "  Index:        $INDEX"
echo "  Dimension:    $DIMENSION"
echo "  Vector count: $VECTOR_COUNT"
echo "  Filter:       $QUERY_FILTER"
if [ -n "$QUERY_TEXT" ]; then
    echo "  Query source: Bedrock embedding (\"$QUERY_TEXT\")"
elif [ -n "$QUERY_FILE" ]; then
    echo "  Query source: file ($QUERY_FILE)"
else
    echo "  Query source: first test data file"
fi
echo "===================================================================="

# ============================================================================
# STEP 0: Optional cleanup
# ============================================================================

if [ "$CLEAN" = true ]; then
    log_step "Cleanup"
    rm -rf /tmp/s3vectors
    mkdir -p /tmp/s3vectors
    log_ok "Removed and recreated /tmp/s3vectors"
fi

# ============================================================================
# STEP 1: Create Vector Bucket
# ============================================================================

log_step "Create Vector Bucket"

result=$(s3v CreateVectorBucket "{\"vectorBucketName\": \"$BUCKET\"}")
bucket_out=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin).get('vectorBucketName',''))" 2>/dev/null)

if [ "$bucket_out" = "$BUCKET" ]; then
    log_ok "Bucket '$BUCKET' created"
else
    log_err "Bucket creation failed: $result"
fi

# ============================================================================
# STEP 2: Create Vector Index with Dynamic Schema
# ============================================================================

log_step "Create Vector Index (dimension=$DIMENSION, dynamic schema)"

result=$(s3v CreateIndex "{
    \"vectorBucketName\": \"$BUCKET\",
    \"indexName\": \"$INDEX\",
    \"dimension\": $DIMENSION,
    \"indexType\": \"IVF_PQ\",
    \"distanceMetric\": \"cosine\",
    \"unindexedThreshold\": 256,
    \"scalarSchema\": [
        {\"name\": \"file_path\",     \"type\": \"string\"},
        {\"name\": \"line_start\",    \"type\": \"int\"},
        {\"name\": \"line_end\",      \"type\": \"int\"},
        {\"name\": \"type\",          \"type\": \"string\"},
        {\"name\": \"function_name\", \"type\": \"string\"},
        {\"name\": \"class_name\",    \"type\": \"string\"}
    ]
}")

schema_count=$(echo "$result" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d.get('config',{}).get('scalarSchema',[])))" 2>/dev/null)

if [ "$schema_count" = "6" ]; then
    log_ok "Index '$INDEX' created with 6 scalar columns"
    echo "  Columns: file_path(string), line_start(int), line_end(int), type(string), function_name(string), class_name(string)"
else
    log_err "Index creation failed: $result"
fi

# ============================================================================
# STEP 3: Get Index State (before insert)
# ============================================================================

log_step "Get Index State (before insert)"

state=$(s3v GetIndexState "{\"vectorBucketName\": \"$BUCKET\", \"indexName\": \"$INDEX\"}")
echo "$state" | python3 -c "
import json, sys
s = json.load(sys.stdin)
print(f'  Rows:                {s.get(\"currentRows\", 0)}')
print(f'  Version:             {s.get(\"currentVersion\", 0)}')
print(f'  Indexed rows:        {s.get(\"numIndexedRows\", \"n/a\")}')
print(f'  Unindexed rows:      {s.get(\"numUnindexedRows\", \"n/a\")}')
print(f'  Rebuild threshold:   {s.get(\"unindexedThreshold\", 0)}')
print(f'  Build in progress:   {s.get(\"indexBuildInProgress\", False)}')
" 2>/dev/null

# ============================================================================
# STEP 4: Put Vectors from Test Data
# ============================================================================

log_step "Put Vectors ($VECTOR_COUNT files from $DATA_DIR)"

put_success=0
put_failed=0

for f in $(ls "$DATA_DIR"/outpref_xx__batch_*.json | sort | head -"$VECTOR_COUNT"); do
    transformed=$(python3 -c "
import json
with open('$f') as fh:
    data = json.load(fh)
data['vectorBucketName'] = '$BUCKET'
data['indexName'] = '$INDEX'
for v in data.get('vectors', []):
    d = v.get('data', {})
    if isinstance(d, dict) and 'float32' in d:
        v['data'] = d['float32']
print(json.dumps(data))
")
    out=$(s3v PutVectors "$transformed")
    if echo "$out" | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if d.get('inserted') else 1)" 2>/dev/null; then
        put_success=$((put_success + 1))
    else
        put_failed=$((put_failed + 1))
    fi

    # Progress indicator
    total=$((put_success + put_failed))
    if [ $((total % 50)) -eq 0 ]; then
        echo "  Progress: $total / $VECTOR_COUNT ..."
    fi
done

echo "  Inserted: $put_success  Failed: $put_failed  Total: $VECTOR_COUNT"

if [ "$put_success" -eq "$VECTOR_COUNT" ]; then
    log_ok "All $VECTOR_COUNT vectors inserted"
else
    log_err "$put_failed vectors failed to insert"
fi

# ============================================================================
# STEP 5: Get Index State (after insert)
# ============================================================================

log_step "Get Index State (after insert)"

state=$(s3v GetIndexState "{\"vectorBucketName\": \"$BUCKET\", \"indexName\": \"$INDEX\"}")
echo "$state" | python3 -c "
import json, sys
s = json.load(sys.stdin)
print(f'  Rows:                {s.get(\"currentRows\", 0)}')
print(f'  Version:             {s.get(\"currentVersion\", 0)}')
print(f'  Indexed rows:        {s.get(\"numIndexedRows\", \"n/a\")}')
print(f'  Unindexed rows:      {s.get(\"numUnindexedRows\", \"n/a\")}')
print(f'  Num indices:         {s.get(\"numIndices\", 0)}')
print(f'  Build in progress:   {s.get(\"indexBuildInProgress\", False)}')
print(f'  Needs rebuild:       {s.get(\"needsRebuild\", False)}')
" 2>/dev/null

# ============================================================================
# STEP 6: Prepare Query Vector
# ============================================================================

log_step "Prepare Query Vector"

if [ -n "$QUERY_TEXT" ]; then
    # Generate embedding via AWS Bedrock
    if [ ! -x "$BEDROCK_SCRIPT" ]; then
        echo "  ERROR: Bedrock script not found at $BEDROCK_SCRIPT"
        exit 1
    fi
    echo "  Generating embedding for: \"$QUERY_TEXT\" ..."
    QUERY_VECTOR=$("$BEDROCK_SCRIPT" "$QUERY_TEXT")
    log_ok "Query vector generated via Bedrock (dimension=$(echo "$QUERY_VECTOR" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))" 2>/dev/null))"

elif [ -n "$QUERY_FILE" ]; then
    # Load from specified file
    QUERY_VECTOR=$(python3 -c "
import json
with open('$QUERY_FILE') as f:
    data = json.load(f)
v = data['vectors'][0]['data']
if isinstance(v, dict) and 'float32' in v:
    v = v['float32']
print(json.dumps(v))
")
    log_ok "Query vector loaded from $QUERY_FILE"

else
    # Use the first test data file
    first_file=$(ls "$DATA_DIR"/outpref_xx__batch_*.json | sort | head -1)
    QUERY_VECTOR=$(python3 -c "
import json
with open('$first_file') as f:
    data = json.load(f)
v = data['vectors'][0]['data']
if isinstance(v, dict) and 'float32' in v:
    v = v['float32']
print(json.dumps(v))
")
    log_ok "Query vector loaded from $(basename "$first_file")"
fi

# ============================================================================
# STEP 7: Semantic Search (no filter)
# ============================================================================

log_step "Semantic Search (no filter, topK=5)"

result=$(s3v QueryVectors "{
    \"vectorBucketName\": \"$BUCKET\",
    \"indexName\": \"$INDEX\",
    \"queryVector\": $QUERY_VECTOR,
    \"topK\": 5,
    \"returnDistance\": true
}")

echo "$result" | python3 -c "
import json, sys
data = json.load(sys.stdin)
vecs = data.get('vectors', [])
print(f'  Results: {len(vecs)}')
print()
for v in vecs:
    parts = []
    if 'type' in v:          parts.append(f'type={v[\"type\"]}')
    if 'class_name' in v:    parts.append(f'class={v[\"class_name\"]}')
    if 'function_name' in v: parts.append(f'func={v[\"function_name\"]}')
    if 'file_path' in v:     parts.append(f'file={v[\"file_path\"]}')
    if 'line_start' in v:    parts.append(f'line={v[\"line_start\"]}')
    extra = '  '.join(parts)
    print(f'    {v[\"key\"]:12s}  dist={v.get(\"distance\",0):>8.4f}  {extra}')
" 2>/dev/null

# ============================================================================
# STEP 8: Semantic Search with Filter
# ============================================================================

log_step "Semantic Search with Filter: $QUERY_FILTER"

result=$(s3v QueryVectors "{
    \"vectorBucketName\": \"$BUCKET\",
    \"indexName\": \"$INDEX\",
    \"queryVector\": $QUERY_VECTOR,
    \"topK\": 10,
    \"returnDistance\": true,
    \"filter\": \"$QUERY_FILTER\"
}")

echo "$result" | python3 -c "
import json, sys
data = json.load(sys.stdin)

# Check for errors
if 'error' in data:
    print(f'  ERROR: {data[\"error\"].get(\"message\", data[\"error\"])}')
    sys.exit(1)

vecs = data.get('vectors', [])
print(f'  Results: {len(vecs)}')
print()
for v in vecs:
    parts = []
    if 'type' in v:          parts.append(f'type={v[\"type\"]}')
    if 'class_name' in v:    parts.append(f'class={v[\"class_name\"]}')
    if 'function_name' in v: parts.append(f'func={v[\"function_name\"]}')
    if 'file_path' in v:     parts.append(f'file={v[\"file_path\"]}')
    if 'line_start' in v:    parts.append(f'line={v[\"line_start\"]}')
    extra = '  '.join(parts)
    print(f'    {v[\"key\"]:12s}  dist={v.get(\"distance\",0):>8.4f}  {extra}')
" 2>/dev/null

# ============================================================================
# Done
# ============================================================================

echo ""
echo "===================================================================="
echo "  Demo complete."
echo "  Data stored in: /tmp/s3vectors/$BUCKET/"
echo "  To run more queries:"
echo "    $S3V QueryVectors '{\"vectorBucketName\": \"$BUCKET\", \"indexName\": \"$INDEX\", \"queryVector\": [...], \"topK\": 5, \"returnDistance\": true}'"
echo "  To run filtered queries, add:"
echo "    \"filter\": \"type = 'class' AND line_start > 50\""
echo "===================================================================="

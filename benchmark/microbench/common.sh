#!/bin/bash
# Common functions for the aggregation microbenchmarks

# Benchmarks must fully consume results, including when invoked by a coding agent.
export DUCKDB_AGENT_MODE=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARIES_DIR="$SCRIPT_DIR/binaries"
RESULTS_DIR="${RESULTS_DIR:-$SCRIPT_DIR/results}"
# Default binary (with all optimizations enabled)
DEFAULT_BINARY="$BINARIES_DIR/duckdb_default"
TEST_DB="$BINARIES_DIR/test_data.duckdb"

# Default parameters
WARMUP_RUNS=${WARMUP_RUNS:-1}
BENCH_RUNS=${BENCH_RUNS:-3}
THREADS=${THREADS:-1}  # Default to single-threaded to avoid partitioning issues

# Query setup commands
QUERY_SETUP="${QUERY_SETUP:-SET pac_mi=0.0; SET threads=4; SET max_memory='30G'; SET max_temp_directory_size='90GB';}"

mkdir -p "$RESULTS_DIR"

median_seconds() {
    sort -n | awk '{a[NR]=$1} END {if (NR % 2) print a[(NR+1)/2]; else if (NR) print (a[NR/2]+a[NR/2+1])/2}'
}

aggregate_profile_seconds() {
    python3 - "$1" <<'PY'
import json
import sys

with open(sys.argv[1]) as file:
    profile = json.load(file)

def aggregate_time(node):
    kind = node.get("operator_type", node.get("type", ""))
    matched = any(name in kind for name in ("UNGROUPED", "GROUP_BY", "AGGREGATE"))
    seconds = node.get("operator_timing", node.get("timing", 0.0)) if matched else 0.0
    count = int(matched)
    for child in node.get("children", []):
        child_seconds, child_count = aggregate_time(child)
        seconds += child_seconds
        count += child_count
    return seconds, count

seconds = 0.0
count = 0
# DuckDB 2.0 places the operator tree under "operator" and renames its fields.
for root in profile.get("operator", [profile]):
    root_seconds, root_count = aggregate_time(root)
    seconds += root_seconds
    count += root_count
if not count:
    raise ValueError("No aggregate operator found in the query profile")
print(f"{seconds:.3f}")
PY
}

# Run a benchmark query and extract timing
# Args: binary, query, [runs]
# Returns: wall_median,agg_median,wall_times,agg_times
run_bench() {
    local binary=$1
    local query=$2
    local runs=${3:-$BENCH_RUNS}

    if [[ ! -f "$binary" ]]; then
        echo "ERROR: Binary not found: $binary" >&2
        return 1
    fi

    # Warmup
    for ((i=0; i<WARMUP_RUNS; i++)); do
        "$binary" -c "$QUERY_SETUP $query" >/dev/null 2>> "$RESULTS_DIR/errors.log"
    done

    # Benchmark runs with profiling
    local wall_times=()
    local agg_times=()
    local profile_dir=$(mktemp -d)
    local profile_file="$profile_dir/profile.json"
    local query_failed=0
    local timeout_threshold=25.0

    for ((i=0; i<runs; i++)); do
        local start=$(python3 -c 'import time; print(time.time())')
        if ! "$binary" -c "
            $QUERY_SETUP
            SET enable_profiling='json';
            SET profiling_output='$profile_file';
            $query
        " >/dev/null 2>> "$RESULTS_DIR/errors.log"; then
            query_failed=1
            break
        fi
        local end=$(python3 -c 'import time; print(time.time())')
        local elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        wall_times+=("$elapsed")

        local agg_time
        if ! agg_time=$(aggregate_profile_seconds "$profile_file" 2>> "$RESULTS_DIR/errors.log"); then
            query_failed=1
            break
        fi
        agg_times+=("$agg_time")

        # Stop repeating if query took longer than threshold
        if (( $(echo "$elapsed > $timeout_threshold" | bc -l) )); then
            break
        fi
    done

    rm -f "$profile_file"
    rmdir "$profile_dir"

    # If any benchmark run failed, return error indicator
    if [[ $query_failed -eq 1 ]]; then
        echo "-1,-1,-1,-1"
        return 0
    fi

    # Calculate medians
    local wall_median=$(printf '%s\n' "${wall_times[@]}" | median_seconds)
    local agg_median=$(printf '%s\n' "${agg_times[@]}" | median_seconds)

    echo "$wall_median,$agg_median,${wall_times[*]},${agg_times[*]}"
}

# Run DuckDB with timing pragma
# Args: binary, query
run_timed() {
    local binary=$1
    local query=$2

    "$binary" -c "
        .timer on
        $query
    " 2>&1
}

# Generate data sizes for benchmarks (in millions)
# Small: 10M, Medium: 100M, Large: 500M, XL: 1B (1000M)
DATA_SIZES_M=(10 50 100 500 1000)

# Group counts for grouped aggregation benchmarks
GROUP_COUNTS=(1 10 100 1000 10000 100000 1000000)

# Get binary for a variant
get_binary() {
    local variant=$1
    echo "$BINARIES_DIR/duckdb_$variant"
}

# Check if binary exists
check_binary() {
    local variant=$1
    local binary=$(get_binary "$variant")
    if [[ ! -f "$binary" ]]; then
        echo "Binary not found: $binary" >&2
        echo "Run: ./build_variants.sh $variant" >&2
        return 1
    fi
}

# Format number with commas
fmt_num() {
    printf "%'d" "$1"
}

# Print benchmark header
print_header() {
    local test_name=$1
    echo ""
    echo "========================================"
    echo "$test_name"
    echo "========================================"
    echo "Date: $(date)"
    echo "Host: $(hostname)"
    echo "========================================"
}

# CSV output helpers
csv_header() {
    echo "test,variant,data_size,groups,dtype,distribution,time_sec,all_times"
}

append_result() {
    local file=$1
    shift
    echo "$@" >> "$file"
}

# Check if test database exists
check_test_db() {
    if [[ ! -f "$TEST_DB" ]]; then
        echo "Test database not found: $TEST_DB" >&2
        echo "Run: ./create_test_db.sh" >&2
        return 1
    fi
}

# Run a benchmark query against the test database
# Args: binary, query, [runs]
# Returns: wall_median,agg_median,wall_times,agg_times
# Returns -1,-1,-1,-1 if query fails (e.g., out of memory)
run_bench_db() {
    local binary=$1
    local query=$2
    local runs=${3:-$BENCH_RUNS}

    if [[ ! -f "$binary" ]]; then
        echo "ERROR: Binary not found: $binary" >&2
        return 1
    fi

    if [[ ! -f "$TEST_DB" ]]; then
        echo "ERROR: Test database not found: $TEST_DB" >&2
        echo "Run: ./create_test_db.sh" >&2
        return 1
    fi

    # Warmup - check if query succeeds
    local warmup_failed=0
    for ((i=0; i<WARMUP_RUNS; i++)); do
        if ! "$binary" "$TEST_DB" -c "$QUERY_SETUP $query" >/dev/null 2>> "$RESULTS_DIR/errors.log"; then
            warmup_failed=1
            break
        fi
    done

    # If warmup failed, return error indicator
    if [[ $warmup_failed -eq 1 ]]; then
        echo "-1,-1,-1,-1"
        return 0
    fi

    # Benchmark runs with profiling
    local wall_times=()
    local agg_times=()
    local profile_dir=$(mktemp -d)
    local profile_file="$profile_dir/profile.json"
    local query_failed=0
    local timeout_threshold=25.0

    for ((i=0; i<runs; i++)); do
        local start=$(python3 -c 'import time; print(time.time())')
        if ! "$binary" "$TEST_DB" -c "
            $QUERY_SETUP
            SET enable_profiling='json';
            SET profiling_output='$profile_file';
            $query
        " >/dev/null 2>> "$RESULTS_DIR/errors.log"; then
            query_failed=1
            break
        fi
        local end=$(python3 -c 'import time; print(time.time())')
        local elapsed=$(python3 -c "print(f'{$end - $start:.3f}')")
        wall_times+=("$elapsed")

        local agg_time
        if ! agg_time=$(aggregate_profile_seconds "$profile_file" 2>> "$RESULTS_DIR/errors.log"); then
            query_failed=1
            break
        fi
        agg_times+=("$agg_time")

        # Stop repeating if query took longer than threshold
        if (( $(echo "$elapsed > $timeout_threshold" | bc -l) )); then
            break
        fi
    done

    rm -f "$profile_file"
    rmdir "$profile_dir"

    # If any benchmark run failed, return error indicator
    if [[ $query_failed -eq 1 ]]; then
        echo "-1,-1,-1,-1"
        return 0
    fi

    # Calculate medians
    local wall_median=$(printf '%s\n' "${wall_times[@]}" | median_seconds)
    local agg_median=$(printf '%s\n' "${agg_times[@]}" | median_seconds)

    echo "$wall_median,$agg_median,${wall_times[*]},${agg_times[*]}"
}

#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 7 ]]; then
	cat >&2 <<'USAGE'
usage: native_run.sh DUCKDB_BUILD_DIRECTORY DATABASE OUTPUT_DIRECTORY [LABEL] [THREADS] [CONNECTIONS] [SECONDS]
USAGE
	exit 2
fi

build_dir=$1
database=$2
output_dir=$3
label=${4:-run}
threads=${5:-1}
connections=${6:-1}
seconds=${7:-5}
cpu_set=${CPU_SET:-48-64}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
runner="$output_dir/native_runner"
cxx=${CXX:-}
if [[ -z "$cxx" && -f "$build_dir/CMakeCache.txt" ]]; then
	cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt" | head -n 1)
fi
cxx=${cxx:-c++}
mkdir -p "$output_dir"

"$cxx" -std=c++17 -O2 -pthread \
	-I"$repo_root/src/include" -I"$build_dir/src/include" \
	"$script_dir/native_runner.cpp" "$build_dir/src/libduckdb.so" \
	-Wl,-rpath,"$build_dir/src" -o "$runner"

for query in point in32 wide medium_plain medium_zstd; do
	output="$output_dir/${label}_${query}_t${threads}_c${connections}.json"
	expected_affinity=$(taskset -c "$cpu_set" bash -c 'taskset -pc $$' 2>/dev/null | sed -E 's/.*: //')
	taskset -c "$cpu_set" "$runner" "$database" "$threads" "$connections" "$query" "$seconds" > "$output" &
	pid=$!
	while kill -0 "$pid" 2>/dev/null; do
		for thread_path in "/proc/$pid"/task/*; do
			thread_id=${thread_path##*/}
			actual_affinity=$(taskset -pc "$thread_id" 2>/dev/null | sed -E 's/.*: //' || true)
			if [[ -n "$actual_affinity" && "$actual_affinity" != "$expected_affinity" ]]; then
				echo "thread $thread_id escaped affinity: expected $expected_affinity, got $actual_affinity" >&2
				kill "$pid" 2>/dev/null || true
				wait "$pid" 2>/dev/null || true
				exit 1
			fi
		done
		sleep 0.05
	done
	wait "$pid"
	python3 - "$output" "$expected_affinity" <<'PY'
import json
import sys

path, expected = sys.argv[1:]
data = json.loads(open(path).read())
if data["affinity"] != expected:
    raise SystemExit(f"{path}: JSON affinity {data['affinity']!r} != verified {expected!r}")
cpu_seconds = data["user_cpu_s"] + data["system_cpu_s"]
observed = cpu_seconds / data["elapsed_s"]
allowed = 0
for part in expected.split(","):
    bounds = [int(value) for value in part.split("-")]
    allowed += bounds[-1] - bounds[0] + 1 if len(bounds) == 2 else 1
if observed > allowed * 1.05 + 0.05:
    raise SystemExit(f"{path}: CPU seconds/wall seconds {observed:.3f} exceeds affinity {allowed}")
PY
done

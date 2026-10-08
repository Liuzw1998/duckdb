#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
	cat >&2 <<'USAGE'
usage: run.sh DUCKDB_BINARY [OUTPUT_DIRECTORY] [LABEL]
USAGE
	exit 2
fi

duckdb_bin=$1
output_dir=${2:-benchmark-results}
label=${3:-patched}
cpu_set=${CPU_SET:-48-64}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output_dir"
database="$output_dir/orders.db"
rm -f "$database"

cp "$script_dir/setup.sql" "$output_dir/setup.sql"

/usr/bin/time -f 'setup_wall_seconds=%e\nsetup_cpu_seconds=%U\nsetup_system_seconds=%S\nsetup_max_rss_kb=%M' \
	-o "$output_dir/setup.time" taskset -c "$cpu_set" "$duckdb_bin" "$database" < "$output_dir/setup.sql" > "$output_dir/setup.out"

cat > "$output_dir/plan.sql" <<'SQL'
EXPLAIN ANALYZE SELECT count(*) FROM orders WHERE id = 596095 AND tenant_id = 45 AND status = 'ready';
EXPLAIN ANALYZE SELECT count(*) FROM orders WHERE id IN (596095, 644070, 692045, 740020, 787995, 835970, 883945, 931920)
  AND status = 'ready';
SELECT count(*) FROM orders WHERE id = 596095 AND tenant_id = 45 AND status = 'ready';
SELECT count(*) FROM orders WHERE id IN (596095, 644070, 692045, 740020, 787995, 835970, 883945, 931920)
  AND status = 'ready';
EXPLAIN ANALYZE SELECT sum(length(payload_wide)) FROM candidate_orders
  WHERE lookup_key = 42 AND status_one = 'ready';
SELECT sum(length(payload_wide)) FROM candidate_orders
  WHERE lookup_key = 42 AND status_one = 'ready';
SQL

make_in32() {
	local first=1
	printf 'EXECUTE q_in32('
	for i in $(seq 12345 12376); do
		local id=$(( (i * 48271) % 1000000 ))
		if [[ $first -eq 0 ]]; then printf ', '; fi
		printf '%s' "$id"
		first=0
	done
	printf ", 'ready');\n"
}

for mode in default table-scan; do
	mode_dir="$output_dir/${label}-${mode}"
	mkdir -p "$mode_dir"
	if [[ "$mode" == table-scan ]]; then settings="SET index_scan_max_count=0; SET index_scan_percentage=0;"; else settings=""; fi
	{
		printf 'SET threads=1;\n%s\n' "$settings"
		cat "$output_dir/plan.sql"
	} > "$mode_dir/plan.sql"
	/usr/bin/time -f 'plan_wall_seconds=%e\nplan_cpu_seconds=%U\nplan_system_seconds=%S\nplan_max_rss_kb=%M' \
		-o "$mode_dir/plan.time" taskset -c "$cpu_set" "$duckdb_bin" "$database" < "$mode_dir/plan.sql" > "$mode_dir/plan.out" 2>&1

	for threads in 1 4; do
		steady_sql="$mode_dir/steady_threads${threads}.sql"
		labels="$mode_dir/steady_threads${threads}.labels"
		: > "$labels"
		{
			printf 'SET threads=%s;\n%s\n' "$threads" "$settings"
			cat <<'SQL'
PREPARE q_point AS SELECT count(*) FROM orders WHERE id = $1 AND tenant_id = $2 AND status = $3;
PREPARE q_in8 AS SELECT count(*) FROM orders WHERE id IN ($1, $2, $3, $4, $5, $6, $7, $8) AND status = $9;
PREPARE q_in32 AS SELECT count(*) FROM orders WHERE id IN ($1, $2, $3, $4, $5, $6, $7, $8,
    $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24,
    $25, $26, $27, $28, $29, $30, $31, $32) AND status = $33;
PREPARE q_candidate_narrow_all AS SELECT sum(length(payload_narrow)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_all = 'ready';
PREPARE q_candidate_narrow_few AS SELECT sum(length(payload_narrow)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_few = 'ready';
PREPARE q_candidate_narrow_one AS SELECT sum(length(payload_narrow)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_one = 'ready';
PREPARE q_candidate_wide_all AS SELECT sum(length(payload_wide)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_all = 'ready';
PREPARE q_candidate_wide_few AS SELECT sum(length(payload_wide)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_few = 'ready';
PREPARE q_candidate_wide_one AS SELECT sum(length(payload_wide)) FROM candidate_orders
    WHERE lookup_key = 42 AND status_one = 'ready';
EXECUTE q_point(596095, 45, 'ready');
EXECUTE q_in8(596095, 644070, 692045, 740020, 787995, 835970, 883945, 931920, 'ready');
.timer on
SQL
			for i in $(seq 12345 12374); do
				id=$(( (i * 48271) % 1000000 )); tenant=$(( i % 100 ));
				if (( i % 10 < 8 )); then status=ready; else status=closed; fi
				printf "EXECUTE q_point(%s, %s, '%s');\n" "$id" "$tenant" "$status"
				echo point >> "$labels"
			done
			for _ in $(seq 1 10); do
				printf "EXECUTE q_in8(596095, 644070, 692045, 740020, 787995, 835970, 883945, 931920, 'ready');\n"
				echo in8 >> "$labels"
			done
			for _ in $(seq 1 10); do
				make_in32
				echo in32 >> "$labels"
			done
			for kind in narrow_all narrow_few narrow_one wide_all wide_few wide_one; do
				for _ in $(seq 1 5); do
					printf 'EXECUTE q_candidate_%s;\n' "$kind"
					echo "candidate_${kind}" >> "$labels"
				done
			done
		} > "$steady_sql"
		/usr/bin/time -f 'process_wall_seconds=%e\nprocess_cpu_seconds=%U\nprocess_system_seconds=%S\nprocess_max_rss_kb=%M' \
			-o "$mode_dir/steady_threads${threads}.process.time" taskset -c "$cpu_set" "$duckdb_bin" "$database" \
			< "$steady_sql" > "$mode_dir/steady_threads${threads}.out" 2>&1
	done
done

python3 - "$output_dir" <<'PY'
import pathlib
import re
import statistics
import sys

root = pathlib.Path(sys.argv[1])
for mode_dir in sorted(root.glob('*-default')) + sorted(root.glob('*-table-scan')):
    for output in sorted(mode_dir.glob('steady_threads*.out')):
        labels = (mode_dir / (output.stem + '.labels')).read_text().splitlines()
        timers = []
        for line in output.read_text(errors='replace').splitlines():
            match = re.search(r'Run Time \(s\): real ([0-9.]+) user ([0-9.]+) sys ([0-9.]+)', line)
            if match:
                timers.append(tuple(float(value) for value in match.groups()))
        if len(timers) != len(labels):
            raise SystemExit(f'{output}: expected {len(labels)} timers, found {len(timers)}')
        rows = []
        for kind in ('point', 'in8', 'in32', 'candidate_narrow_all', 'candidate_narrow_few',
                     'candidate_narrow_one', 'candidate_wide_all', 'candidate_wide_few',
                     'candidate_wide_one'):
            values = [row for row, name in zip(timers, labels) if name == kind]
            p50 = [statistics.median(row[i] for row in values) for i in range(3)]
            p95 = [sorted(row[i] for row in values)[max(0, int(len(values) * 0.95) - 1)] for i in range(3)]
            rows.append((kind, len(values), *p50, *p95))
        (mode_dir / (output.stem + '.summary.tsv')).write_text(
            'query\tcount\tp50_real_s\tp50_user_s\tp50_sys_s\tp95_real_s\tp95_user_s\tp95_sys_s\n' +
            ''.join('\t'.join(map(str, row)) + '\n' for row in rows))
PY

printf 'binary=%s\nlabel=%s\ncpu_set=%s\nrows=1000000\nseed=48271\nthreads=1,4\n' "$duckdb_bin" "$label" "$cpu_set" > "$output_dir/environment"

# Multiple-filter ART lookup report

## Workload and measurement

Baseline (B) and patch (P) were built independently from base SHA
`8681dc52536466c7ea119b9dd3b199fa943b3c16`, with the same Release options. Each build used its own
database snapshot. The native runner opens one DuckDB instance and creates all requested connections in
that process; it prepares statements and warms each connection before a five-second measurement interval.
The measured interval consumes every result chunk. Process CPU is reported as

\[
\frac{\Delta(\text{user CPU}+\text{system CPU})}{\text{completed queries}}
\]

with one `getrusage` interval around the whole shared process. The runner is started with CPU affinity
`48-64`; the wrapper verifies every worker thread's affinity and the JSON records the observed mask.
Before timing, the runner verifies all 30 point results, the IN(32) count of 26, and the one 4-KiB wide
payload row. The JSON also retains `user_cpu_s`, `system_cpu_s`, and `elapsed_s`; every measured cell
stayed below the 17-CPU affinity bound.

The main table has one million rows with this schema and index:

```sql
CREATE TABLE orders(
    payload VARCHAR,
    id BIGINT PRIMARY KEY,
    tenant_id INTEGER,
    status VARCHAR,
    amount INTEGER
);
```

`id` is generated as `(i * 48271) % 1000000`; `tenant_id` is `i % 100`, and `status` is `ready` for
`i % 10 < 8`, otherwise `closed`. The point query is:

```sql
SELECT count(*) FROM orders WHERE id = ? AND tenant_id = ? AND status = ?;
```

The complete IN(32) query is:

```sql
SELECT count(*) FROM orders
WHERE id IN (?, ?, ?, ?, ?, ?, ?, ?,
             ?, ?, ?, ?, ?, ?, ?, ?,
             ?, ?, ?, ?, ?, ?, ?, ?,
             ?, ?, ?, ?, ?, ?, ?, ?)
  AND status = ?;
```

Point parameters cycle through `i = 12345..12374` and produce one candidate and one output row. The
IN(32) parameter list uses `i = 12345..12376`, producing 32 ART candidates and 26 rows after the residual
`status` predicate.

## Steady-state results

Single-connection native results (one five-second run per cell; p50/p95 are the per-query distribution
within that run) are:

| Query | threads | B p50/p95 | P p50/p95 | B CPU/query | P CPU/query |
| --- | ---: | ---: | ---: | ---: | ---: |
| Point + residual | 1 | 4.234/4.313 ms | **0.714/0.739 ms** | 4.262 ms | **0.721 ms** |
| Point + residual | 4 | 1.864/2.032 ms | **0.699/0.735 ms** | 5.325 ms | **0.962 ms** |
| IN(32) + residual | 1 | 12.041/12.144 ms | **1.384/1.414 ms** | 12.118 ms | **1.422 ms** |
| IN(32) + residual | 4 | 4.204/4.291 ms | **1.360/1.407 ms** | 12.863 ms | **1.782 ms** |

B's default plan is `Sequential Scan`; P's default plan is `Index Scan`. On these workloads P lowers
both latency and CPU/query. Four execution threads help B's full scan, while the indexed point and IN
queries are already close to their single-connection low latency at one thread.

The shared-instance concurrency run uses one DuckDB instance configured with `threads=1`, driven by eight
application threads, each owning one connection:

| Query | connections | B QPS / p95 | P QPS / p95 | B CPU/query | P CPU/query |
| --- | ---: | ---: | ---: | ---: | ---: |
| Point + residual | 1 | 236 / 4.31 ms | **1,394 / 0.74 ms** | 4.262 ms | **0.721 ms** |
| Point + residual | 8 | 1,854 / 4.52 ms | **10,547 / 0.83 ms** | 4.319 ms | **0.752 ms** |
| IN(32) + residual | 1 | 83 / 12.14 ms | **719 / 1.41 ms** | 12.118 ms | **1.422 ms** |
| IN(32) + residual | 8 | 663 / 12.58 ms | **5,485 / 1.55 ms** | 12.077 ms | **1.437 ms** |

The connections=8 rows are not eight independent CLI processes. The one-process CPU interval and the
completion count use the same measurement boundary, so the CPU/query values are not copied from the
single-connection rows.

These are warmed hot-key lookups: every point-query worker cycles through the same 30 keys, and every
IN(32) worker repeats the same 32-key list. The results describe this selective hot-key workload rather
than random primary-key access across the full table.

## Fetch-first materialization cost

The auxiliary `candidate_orders` table has 10,000 rows: 1,500 rows share `lookup_key = 42`, 8,500 are
background rows, and an ART is created on `lookup_key`. It has 128-byte and 4-KiB payload columns. The
`status_all`, `status_few`, and `status_one` residuals let the experiment retain all 1,500, 10, or 1
candidate rows.

The implementation is Fetch-first: for a persistent ART candidate batch, `DataTable::Fetch` receives the
complete scan-input column set before residual filters are applied. Therefore reducing residual pass rate
does not reduce the number of candidates submitted to this Fetch. The CLI diagnostic values below are
rounded single-invocation wall times; `S` is the patch binary with both index thresholds set to zero, so
it is a sequential-scan control rather than a third implementation. These diagnostics were refreshed with
the same `48-64` affinity; their millisecond precision is still too coarse for stage-level attribution.

| Query | B (default sequential) | P (default index) | S (P, both thresholds 0) |
| --- | ---: | ---: | ---: |
| Narrow, 1,500 rows pass | 0.001 s | 0.003 s | 0.001 s |
| Narrow, 10 or 1 row passes | 0.001 s | 0.003 s | 0.001 s |
| Wide, 1,500, 10, or 1 row passes | 0.001 s | 0.003 s | 0.001 s |

The native runner's one-row wide-payload query (`lookup_key = 42 AND status_one = 'ready'`) provides a
steady-state confirmation of the negative case: B p50/p95 `0.646/0.662 ms`, P `3.154/3.727 ms`, with
CPU/query `0.646 ms` versus `3.370 ms`. This establishes a Fetch-first regression signal for this
workload, but does not isolate payload width, ART recheck, filtering, or storage layout. It also does not
justify a precise three-times attribution for the rounded CLI values. The guarded build G below addresses
this case by rejecting the expensive candidate admission and taking the sequential path. A two-stage Fetch
is not implemented.

## Codec-aware admission guard

The guard applies only when there are multiple single-column filters. It keeps the existing candidate cap

```text
M = max(index_scan_max_count, index_scan_percentage * total_rows)
```

and runs after the ART has collected and normalized its final candidate row IDs. It inspects only the
`input.column_indexes` that the actual Fetch will read. Row IDs are ordered, so the guard reuses the current
RowGroup and ColumnSegment while walking candidates; it does not initialize a decoder or scan data. Nested,
virtual, pushdown-extract, otherwise unresolvable, and non-VARCHAR scalar mappings are skipped; the current
guarded codecs are VARCHAR segment codecs only.

The initial calibrated factors are deliberately conservative:

| Actual fetched segment codec | factor | effective cap when M=2048 |
| --- | ---: | ---: |
| ordinary codecs | 1 | 2,048 |
| DICT_FSST | 1/32 | 64 |
| ZSTD | 1/32 | 64 |

If several touched segments use guarded codecs, the smallest effective cap applies. A rejected ART is
discarded and the loop continues to a later ART; only when none passes does the scan fall back to Sequential
Scan. Single-filter ART admission is unchanged.

The calibration used one-shot JSON operator profiles on 10,000-row tables with 1, 32, 128, 512, and 1,500
candidates. DICT_FSST point-fetch operator time was approximately `0.30/0.34/0.45/0.89/2.82 ms`, while
the sequential control was `0.31/0.32/0.34/0.44/0.33 ms`. ZSTD was approximately
`0.44/2.00/7.68/54.50/351.54 ms`, against a `5.12–5.31 ms` sequential control. These are diagnostic
single executions, not formal medians; the 1/32 factor leaves headroom around the observed crossover and
may conservatively reject a borderline case.

The key B/P/G check used the same prepared runner and `48-64` affinity:

| Query, threads=1 | B baseline | P current patch | G with guard |
| --- | ---: | ---: | ---: |
| Point p50 / CPU per query | 4.234 / 4.262 ms | 0.714 / 0.721 ms | 0.718 / 0.730 ms |
| IN(32) p50 / CPU per query | 12.041 / 12.118 ms | 1.384 / 1.422 ms | 1.398 / 1.419 ms |
| 1,500-candidate wide p50 / CPU per query | 0.646 / 0.646 ms | 3.154 / 3.370 ms | 0.711 / 0.725 ms |

G uses `Index Scan` for the point and IN(32) workloads and `Sequential Scan` for the 1,500-candidate
DICT_FSST negative case. B/P are the independent Release pair; G is the independent Release build from the
current guarded source. The G numbers are one five-second run per cell, so they show path preservation and
negative-case direction rather than a new statistical benchmark claim. In this run G's wide-query p50 was
about `0.065 ms` above B; that difference includes guard/probe and run noise and is not an isolated guard
microbenchmark.

The review follow-up adds two one-run targeted cases, both with `48-64` affinity: 128 candidates in a
100,000-row table. `medium_plain` stores the payload with Uncompressed segments and should keep the index
path; `medium_zstd` stores the payload with ZSTD and should exercise the guarded rejection path. Values are
p50 / CPU per query in milliseconds:

| Query | B baseline (Seq) | P current patch | G with guard |
| --- | ---: | ---: | ---: |
| 128 candidates, Uncompressed | 0.675 / 0.675 | 0.704 / 0.738 | 0.714 / 0.738 |
| 128 candidates, ZSTD | 1.315 / 1.370 | 7.554 / 7.645 | 1.339 / 1.396 |

The plain case shows a small metadata-walk cost in G relative to P in this run. The ZSTD case shows P's
Fetch-first regression and G's rejection back to the sequential path. These are targeted path and direction
checks from one five-second run, not a stable speedup claim or a replacement for a larger performance matrix.

## Implementation boundary

The patch removes the `FilterCount() != 1` selection guard, but still uses only the first successful
single-column ART that passes the codec admission. It does not intersect indexes or add a cost model. The
candidate limit is applied before residual filtering and before codec admission.

After `DataTable::Fetch`, all single-column table filters are applied to the complete scan-input chunk
through the existing thread-local `ScanFilterInfo` and `ColumnSegment::FilterSelection`, sharing one
selection vector. Filter-only columns are removed only after that pass. Multi-column expressions remain
above the scan, and LocalStorage keeps its existing complete filtering path. The original single-filter
ART path is preserved without predicate-consumption tracking.

## Reproduction and provenance

The repository contains the setup SQL, CLI runner, and native runner in
`benchmark/multiple_column_art_scan/`. For separate baseline and patch runs:

```sh
CPU_SET=48-64 benchmark/multiple_column_art_scan/run.sh /path/to/build-B/duckdb /tmp/mca-B B
CPU_SET=48-64 benchmark/multiple_column_art_scan/run.sh /path/to/build-P/duckdb /tmp/mca-P P
CPU_SET=48-64 benchmark/multiple_column_art_scan/native_run.sh \
  /path/to/build-B /tmp/mca-B/orders.db /tmp/mca-B/native B 1 8 5
CPU_SET=48-64 benchmark/multiple_column_art_scan/native_run.sh \
  /path/to/build-P /tmp/mca-P/orders.db /tmp/mca-P/native P 1 8 5
CPU_SET=48-64 benchmark/multiple_column_art_scan/native_run.sh \
  /path/to/build-G /tmp/mca-P/orders.db /tmp/mca-G/native G 1 8 5
```

`native_run.sh` links `native_runner.cpp` against the selected build's `src/libduckdb.so`; it does not
reuse a binary or in-memory database from the other build. The benchmark build used GCC 11.2.1,
`DUCKDB_EXPLICIT_PLATFORM=linux_amd64`, and Release configuration. The binary SHA256 receipt and raw
native JSON are retained with the task's external benchmark artifacts under `native-results-gated-48-64/`; the
source package above is the portable reproduction entry point. The G release JSON and codec calibration
profiles are retained with the codec-admission scratch results. `summarize_native.py` validates the same
affinity and CPU bound while formatting every matching B/P/G point, IN(32), and wide JSON row, including
results stored below subdirectories.

## Validation

- B/P independent builds and shared-library link passed.
- The guarded source Release G build and shared-library link passed.
- The B/P/G native checks were run with verified `48-64` affinity; all correctness gates passed and every
  JSON stayed within the 17-CPU bound.
- The codec-admission test passed 120 assertions; the previous three multi-filter tests passed 149 assertions.
- The current Release and GCC 11 ASAN/UBSAN ART scan runs passed 635 assertions; one existing `require tpch` test was skipped.
- `git diff --check`, shell syntax checks, and clang-format checks passed.

Coverage includes residual predicates that all, some, or no candidates pass; duplicate and non-contiguous
IN values; NULL three-valued logic; filter-only and reordered projections; generated, rowid, nested and
multi-column residual expressions; empty multi-batches; candidate limits; later-index fallback; local
transaction rows; UPDATE/DELETE correctness; rollback/commit; snapshot readers; checkpoint/restart; and
compound-index fallback. Codec coverage includes actual DICT_FSST/ZSTD/Uncompressed persistence, equal and
over-limit boundaries, unreferenced wide columns, same-column mixed segments, and later-ART fallback. The
current UPDATE/DELETE planner path is still a sequential scan, so those cases verify write correctness
without claiming new ART-driven DML coverage.

Candidate concentrated-versus-scattered layout, the full sanitizer suite, and the full test suite remain unrun.

The bounded conclusion is that selective hot-key lookups benefit substantially, while the 1,500-candidate
lookup with a highly selective residual predicate regresses under the current Fetch-first path.

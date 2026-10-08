# Multiple-filter ART lookup report

## Current hot-path follow-up (2026-10-08)

The small implementation changes improve both ordinary-codec layouts by about 2% in the three-round
comparison below. They do not eliminate the concentrated wide-payload regression: optimized Index Scan
is still 7.8% slower than same-source Sequential Scan, with 11.2% higher CPU/query. Scattered candidates
retain a 1.30 times p50 speedup over Sequential Scan. The point result is effectively unchanged.

Simple comparisons and BETWEEN now initialize the existing ART scan directly, avoiding temporary key
chunks and value sets. Probes borrow column IDs and copy index expressions only after finding a matching
filter. Codec admission uses one integer `max_count / 32` limit, checks each touched segment once, and
skips its remaining sorted candidates. It allocates no temporary column list and obtains the RowGroup
tree only after finding a supported VARCHAR column. Fetch still reads all candidates and scanned columns;
filter selection uses the complete fetched chunk and slices only the projected output. Admission policy,
full filter rechecks, index-entry protection and vacuum-lock lifetime are retained. Production code is
23 lines shorter than H; this does not introduce two-stage Fetch or a cost model.

H is the frozen implementation from `f6ed92d646`; O includes these optimizations; S is O with both index
thresholds zero. All use base `3b3312a963`, GCC 11 Release and the same common objects and frozen databases
as the lock-scope follow-up below. O's `table_scan.cpp` SHA256 is
`8f974d831d2e50d4393a11381935a33f0c90ad6f62ac178d25a7fee8b8bf6dd4`.
The measurements and sanitizer run precede the final checked cast to the iterator's signed
`difference_type`; Release regression and Clang 20 Tidy checks cover that final type adjustment.
Every cell has one excluded five-second warm-up and three five-second measurements, with rotating order,
one connection and verified per-thread affinity. Actual loaded libraries, unchanged input/source hashes,
correctness gates and actual wall/CPU boundaries are checked. The table contains arithmetic means of the
three interval metrics, in milliseconds except QPS.

| Query / threads / layout / control | p50 | p95 | CPU/query | QPS |
| --- | ---: | ---: | ---: | ---: |
| Plain 128 / 1 / concentrated / H | 0.715 | 0.747 | 0.742 | 1360.9 |
| Plain 128 / 1 / concentrated / O | 0.702 | 0.731 | 0.727 | 1384.9 |
| Plain 128 / 1 / concentrated / S | 0.651 | 0.678 | 0.654 | 1529.3 |
| Plain 128 / 1 / scattered / H | 0.771 | 0.807 | 0.804 | 1262.7 |
| Plain 128 / 1 / scattered / O | 0.756 | 0.802 | 0.788 | 1284.9 |
| Plain 128 / 1 / scattered / S | 0.985 | 1.013 | 0.991 | 1009.9 |
| Point / 1 / concentrated / H | 0.723 | 0.758 | 0.737 | 1373.4 |
| Point / 1 / concentrated / O | 0.724 | 0.747 | 0.729 | 1378.7 |
| IN(32) / 4 / concentrated / H | 1.435 | 1.513 | 1.940 | 690.9 |
| IN(32) / 4 / concentrated / O | 1.499 | 1.560 | 2.006 | 666.2 |
| ZSTD 128 / 1 / concentrated / H | 1.356 | 1.590 | 1.415 | 710.5 |
| ZSTD 128 / 1 / concentrated / O | 1.380 | 1.614 | 1.433 | 701.2 |

This run uses CPUs `48-64`. Ordinary-codec p50 and CPU/query improve in all three rounds for both layouts;
concentrated p50 is H `0.731/0.706/0.707` versus O `0.716/0.697/0.694`, and scattered p50 is
H `0.752/0.752/0.807` versus O `0.739/0.737/0.791`. Three rounds on a shared host do not establish
statistical significance. IN(32) mean p50 regresses 4.5%, with two slower rounds and one faster round;
ZSTD regresses 1.8% while still taking Sequential Scan. These results are retained rather than replaced
by the confirmation below. H/O mean peak RSS differs by less than 0.5 MiB per scenario, with overall H/O peaks
of 58.6-85.2 MiB; those peaks include premeasurement correctness gates.

The host topology shows that CPU 64 is on another socket and shares a physical core with CPU 0, while
CPUs 48-63 are distinct cores on one socket. Some intervals above overlap this task's build/validation
on CPUs 0-31. An independent confirmation therefore uses `48-63`, after those processes exit, with the
same binaries, inputs and warm-up/three-round protocol. Both affinity and concurrent activity change,
so this cannot identify the cause of the earlier regressions. Shared-host activity remains uncontrolled.

| Query / threads / control, CPUs 48-63 | p50 | p95 | CPU/query | QPS |
| --- | ---: | ---: | ---: | ---: |
| IN(32) / 1 / H | 1.384 | 1.423 | 1.396 | 719.9 |
| IN(32) / 1 / O | 1.375 | 1.409 | 1.387 | 724.4 |
| IN(32) / 4 / H | 1.372 | 1.427 | 1.778 | 724.5 |
| IN(32) / 4 / O | 1.363 | 1.415 | 1.767 | 730.1 |
| ZSTD 128 / 1 / H | 1.344 | 1.572 | 1.403 | 716.1 |
| ZSTD 128 / 1 / O | 1.334 | 1.582 | 1.388 | 724.1 |

IN(32) p50 changes by -0.64% at both thread counts; this is too small to claim a meaningful speedup.
Four threads provide little latency benefit here, but O's CPU/query rises about 27% versus one thread.
ZSTD p50 changes by -0.77% and p95 by +0.62%, also not evidence of a substantial improvement.
The confirmation does not prove zero overhead or erase the original `48-64` measurements. Avoiding the
remaining 128-row wide-payload work requires a broader Fetch change; tightening all ordinary-codec
admission would also reject the measured scattered positive case.

The final Release checks pass 1,863 assertions: ART scan and expression-index cases, plus focused
16-KiB, storage-restart, vector-size-512, dictionary-expression and constant-operator configurations.
Assertions + ASAN/UBSAN pass 718 assertions in 23 cases, with the existing TPCH requirement skip.
SQL coverage now includes the non-divisible codec-cap boundary and a sole guarded candidate at the next
RowGroup's first row. These are focused passes, not a full CI gate. Raw evidence is retained separately
under `mca-perf-20261008/measure-20261008T151127.002940Z` and
`mca-perf-20261008/confirm-20261008T152133.180021Z`, including all rounds, wall/CPU seconds, RSS, loaded
libraries, source/input hashes and topology. The interrupted preliminary run and one-second pilot are
excluded. Formatting and whitespace checks pass.

## Earlier lock-scope and locality follow-up (2026-10-08)

The current same-base measurements confirm the point and IN(32) benefits, while preserving the ordinary-codec
regression boundary. At threads=1, mean p50 speedup `T_B / T_G` is 5.65 for point + residual and 8.42 for
IN(32) + residual; at threads=4 it is 2.65 and 3.34. The 128-candidate Uncompressed case remains 11.5% slower
than same-source Sequential Scan with concentrated row IDs, but is 1.33 times faster with scattered row IDs.
The codec guard improves ZSTD mean p50 over unguarded P by 5.47 times concentrated and 2.95 times scattered.
These results support a heuristic limiting known costly point-fetch codecs, not a guarantee against regressions.

These measurements were rebuilt and rerun after rebase. The pre-rebase `a770db1197` review records remain
in their original external task; the tables below use the new base throughout.

All variants retain base `3b3312a963c693efe6bcaef4ee4484b4adc17200`, GCC 11 and the same Release compile/link
options and common objects. B substitutes the base `table_scan.cpp`; P disables only codec admission; G uses
the complete patch, including the ordered index-entry snapshot and entry-locked optional read handle. S uses
G with both index thresholds zero on every connection. The additional header method adds no fields or layout
change and is unused by B. Actual loaded libraries, source/header hashes and compile/link commands are recorded
in the new external task's `controls/manifest.json`; data and runner hashes are in `native-manifest.json`.

The frozen million-row `orders` data and point/IN parameters are unchanged. The concentrated input is the
retained database; scattered tables are regenerated from those same rows with the current build. Point queries produce one ART
candidate; IN(32) produces 32 and returns 26 rows. B uses Sequential Scan and G uses Index Scan in both cases.
These candidate counts take the guard's fast admission branch. The numerical IN keys correspond to consecutive
physical row IDs 12345–12376, so this positive case does not establish scattered Fetch performance.

For the medium cases, each table has 100,000 rows, 128 candidates, one matching residual and one 4-KiB result.
Non-candidate keys cycle through 0–40. Concentrated candidates occupy row IDs 0–127. The scattered copy sorts
the original rows by `(rowid * 71631) % 100000`; bidirectional `EXCEPT ALL` verifies an identical logical row
multiset. The same `medium_plain`/`medium_zstd` SQL runs against both copies, with threads=1, connections=1,
`index_scan_max_count=2048` and percentage=0. `setup.sql` now matches the frozen non-candidate key distribution,
and `scatter.sql` reproduces the physical reorder without regenerating logical rows.

| Layout / codec | Candidate vectors of 2048 rows | Touched payload segments / total | Touched residual segments / total | RowGroups |
| --- | ---: | ---: | ---: | ---: |
| Concentrated / Uncompressed | 1 | 1 / 51 | 1 / 4 | 1 |
| Scattered / Uncompressed | 49 | 50 / 51 | 4 / 4 | 1 |
| Concentrated / ZSTD | 1 | 1 / 1 | 1 / 1 | 1 |
| Scattered / ZSTD | 49 | 1 / 1 | 1 / 1 | 1 |

P/G use Index Scan for Uncompressed. P uses Index Scan for ZSTD; G/S use Sequential Scan. Physical order changes
segment composition, zonemaps and residual locality as well as Fetch locality, so the layout difference cannot
be attributed solely to random payload access. The separate cold check below covers multiple RowGroups.

Every cell has one excluded five-second warm-up and three measured five-second intervals, rotating variant
and layout order. Input hashes are checked again after timing. Every worker passes the correctness gate; live threads are checked against CPU affinity
`48-64`. Actual wall time includes the final worker join, and CPU covers those completed queries. This task's
concurrent build/validation processes use CPUs 0–31; the host is shared. Every raw triplet below is followed
by its arithmetic mean. Latency and CPU/query are milliseconds; QPS is queries/second.

### Point and IN(32) positives

| Query / threads / control | p50: runs → mean | p95: runs → mean | CPU/query: runs → mean | QPS: runs → mean |
| --- | --- | --- | --- | --- |
| Point / 1 / B | 4.248 / 4.318 / 4.211 → 4.259 | 4.490 / 4.415 / 4.306 → 4.404 | 4.297 / 4.351 / 4.254 → 4.301 | 233.9 / 231.0 / 236.9 → 233.9 |
| Point / 1 / G | 0.731 / 0.766 / 0.765 → 0.754 | 0.759 / 0.788 / 0.791 → 0.779 | 0.738 / 0.770 / 0.769 → 0.759 | 1361.8 / 1305.5 / 1305.8 → 1324.4 |
| Point / 4 / B | 1.802 / 1.931 / 1.801 → 1.844 | 1.971 / 2.286 / 1.984 → 2.080 | 5.148 / 5.248 / 5.147 → 5.181 | 550.8 / 505.5 / 545.0 → 533.8 |
| Point / 4 / G | 0.694 / 0.697 / 0.699 → 0.697 | 0.743 / 0.726 / 0.720 → 0.730 | 0.951 / 0.950 / 0.947 → 0.949 | 1429.1 / 1427.1 / 1425.2 → 1427.1 |
| IN(32) / 1 / B | 11.960 / 11.436 / 11.565 → 11.654 | 12.190 / 11.558 / 11.850 → 11.866 | 12.057 / 11.690 / 11.847 → 11.865 | 83.4 / 87.2 / 86.1 → 85.6 |
| IN(32) / 1 / G | 1.396 / 1.375 / 1.380 → 1.384 | 1.440 / 1.409 / 1.415 → 1.421 | 1.430 / 1.402 / 1.391 → 1.408 | 713.0 / 724.5 / 722.6 → 720.0 |
| IN(32) / 4 / B | 6.039 / 3.982 / 4.027 → 4.683 | 7.197 / 4.065 / 4.109 → 5.124 | 14.621 / 12.087 / 12.122 → 12.943 | 169.7 / 250.2 / 247.6 → 222.5 |
| IN(32) / 4 / G | 1.375 / 1.371 / 1.462 → 1.403 | 1.436 / 1.471 / 1.592 → 1.500 | 1.781 / 1.787 / 2.009 → 1.859 | 722.2 / 720.7 / 676.2 → 706.4 |

### Medium candidates and codec admission

| Layout / codec / control | p50: runs → mean | p95: runs → mean | CPU/query: runs → mean | QPS: runs → mean |
| --- | --- | --- | --- | --- |
| Concentrated / plain / P | 0.766 / 0.712 / 0.708 → 0.729 | 0.801 / 0.791 / 0.744 → 0.779 | 0.786 / 0.742 / 0.739 → 0.756 | 1278.2 / 1355.2 / 1372.4 → 1335.2 |
| Concentrated / plain / G | 0.763 / 0.723 / 0.716 → 0.734 | 0.821 / 0.750 / 0.748 → 0.773 | 0.788 / 0.746 / 0.740 → 0.758 | 1275.3 / 1347.2 / 1359.3 → 1327.3 |
| Concentrated / plain / S | 0.656 / 0.662 / 0.656 → 0.658 | 0.679 / 0.689 / 0.686 → 0.684 | 0.659 / 0.665 / 0.661 → 0.662 | 1517.3 / 1504.2 / 1514.7 → 1512.1 |
| Scattered / plain / P | 0.746 / 0.801 / 0.739 → 0.762 | 0.809 / 0.879 / 0.771 → 0.820 | 0.784 / 0.826 / 0.781 → 0.797 | 1298.8 / 1215.3 / 1313.2 → 1275.8 |
| Scattered / plain / G | 0.753 / 0.752 / 0.751 → 0.752 | 0.826 / 0.798 / 0.784 → 0.803 | 0.797 / 0.795 / 0.787 → 0.793 | 1285.1 / 1288.2 / 1293.1 → 1288.8 |
| Scattered / plain / S | 0.979 / 1.026 / 0.992 → 0.999 | 1.003 / 1.053 / 1.017 → 1.025 | 0.982 / 1.031 / 0.997 → 1.003 | 1018.4 / 970.8 / 1003.1 → 997.4 |
| Concentrated / ZSTD / P | 7.643 / 7.715 / 7.722 → 7.693 | 7.798 / 8.054 / 7.978 → 7.943 | 7.715 / 7.810 / 7.799 → 7.775 | 130.2 / 128.6 / 128.8 → 129.2 |
| Concentrated / ZSTD / G | 1.411 / 1.407 / 1.405 → 1.408 | 1.665 / 1.660 / 1.662 → 1.662 | 1.471 / 1.469 / 1.465 → 1.468 | 683.0 / 683.7 / 685.4 → 684.1 |
| Concentrated / ZSTD / S | 1.399 / 1.339 / 1.397 → 1.378 | 1.639 / 1.571 / 1.636 → 1.616 | 1.445 / 1.387 / 1.447 → 1.426 | 692.2 / 721.5 / 691.5 → 701.7 |
| Scattered / ZSTD / P | 6.874 / 6.846 / 7.024 → 6.915 | 7.364 / 7.357 / 7.537 → 7.419 | 6.945 / 6.927 / 7.104 → 6.992 | 144.7 / 145.1 / 141.4 → 143.7 |
| Scattered / ZSTD / G | 2.340 / 2.340 / 2.340 → 2.340 | 2.502 / 2.446 / 2.446 → 2.465 | 2.371 / 2.368 / 2.383 → 2.374 | 423.8 / 424.5 / 424.5 → 424.2 |
| Scattered / ZSTD / S | 2.336 / 2.335 / 2.329 → 2.333 | 2.439 / 2.439 / 2.433 → 2.437 | 2.351 / 2.350 / 2.343 → 2.348 | 425.6 / 425.8 / 427.0 → 426.1 |

Plain G remains 14.6% more expensive in CPU/query than S for concentrated candidates. Scattered G preserves
an admitted, greater-than-64-candidate Index Scan with p50 speedup 1.33 and CPU/query speedup 1.27 over S.
Ordinary G minus P mean CPU/query is +0.002/-0.004 ms concentrated/scattered; p50 changes by
+0.7%/-1.3%. These small differences and round variability do not establish a stable metadata-walk cost estimate.
G's concentrated ZSTD p95 is 1.662 versus S's 1.616 ms. Recovery of the Sequential Scan direction
does not imply equality on every metric.

### Single-filter control

Both B/G use Index Scan for the same 30 single-filter keys, with threads=1. This compares the complete
patch against B rather than isolating the new snapshot allocation.

| Connections / control | p50: runs → mean | p95: runs → mean | CPU/query: runs → mean | QPS: runs → mean |
| --- | --- | --- | --- | --- |
| 1 / B | 0.597 / 0.596 / 0.598 → 0.597 | 0.894 / 0.618 / 0.627 → 0.713 | 0.622 / 0.599 / 0.601 → 0.608 | 1615.2 / 1677.0 / 1670.4 → 1654.2 |
| 1 / G | 0.558 / 0.600 / 0.559 → 0.572 | 0.576 / 0.709 / 0.575 → 0.620 | 0.570 / 0.613 / 0.570 → 0.584 | 1781.8 / 1640.2 / 1779.6 → 1733.9 |
| 8 / B | 0.573 / 0.583 / 0.587 → 0.581 | 0.590 / 0.687 / 0.656 → 0.644 | 0.571 / 0.599 / 0.590 → 0.587 | 13928.9 / 13174.4 / 13335.0 → 13479.5 |
| 8 / G | 0.577 / 0.577 / 0.573 → 0.576 | 0.897 / 0.633 / 0.630 → 0.720 | 0.605 / 0.584 / 0.583 → 0.591 | 13217.9 / 13679.7 / 13704.3 → 13534.0 |

At one connection, G/B mean p50 changes by -4.1% and CPU/query by -3.8%; at eight connections mean p50
changes by -0.9% and QPS by +0.4%. These controls and their round variability do not prove zero overhead or isolate
a snapshot-allocation speedup. Peak RSS over all 72 measured intervals is 58.5–86.0 MiB, including premeasurement
gates. Raw JSON, wall/CPU seconds, RSS and verified thread counts are in the new external task's
`native/run1`–`run3`; actual plans are in `plans` and full precision summaries in `native-summary.json`.
Warm-up records are excluded.

### Cold metadata and entry lifetime

`TableScanInitGlobal` now copies the eligible ART entries in list order, releases the index-list mutex,
and acquires a read handle under each entry's own lock only for its probe. `TryGetReadHandle` checks that
the physical index is still bound while holding that lock. Codec admission runs after both locks are
released; rejecting an ART continues to later snapshot entries. A copied `shared_ptr<IndexEntry>` retains
the logical entry, while concurrent `Retire` may still destroy its physical index, so entry-locked validation
is required. The existing shared vacuum lock continues through Fetch and scan-state destruction.

The local cold fixture persists 8192 rows in four RowGroups of 2048 rows. Its 128 candidates occupy row IDs
0,64,...,8128, with an INTEGER residual, Uncompressed VARCHAR payload and one matching 4-KiB row. Each run
opens a fresh DuckDB instance after closing the setup instance, starts a reader and INSERT on separate
connections, checks actual overlap and verifies both results. Cold refers to DuckDB's metadata state;
the OS page cache was not flushed. Excluding one warm-up pair, natural wall times are:

| Implementation | First query ms: runs → mean | INSERT ms: runs → mean |
| --- | --- | --- |
| Previous list-locked guard | 32.289 / 35.723 / 32.131 → 33.381 | 12.126 / 24.392 / 12.132 → 16.217 |
| Current entry snapshot | 38.016 / 32.995 / 33.781 → 34.931 | 24.638 / 11.562 / 11.918 → 16.040 |

The natural samples do not establish a stable write-latency improvement. A separate local-only diagnostic
pauses the first actual lazy payload-column load immediately before `MetadataReader` construction. Both
versions subsequently load all four RowGroups in the guard. Across three runs, the old version's INSERT
cannot complete during the 300-ms controlled pause; the current version's INSERT completes before release
in all three. This demonstrates the removed list-lock coupling, not a measured cold-I/O speedup or a newly
reproduced deadlock. No diagnostic hooks enter the committed implementation.

A second controlled check uses a broad ZSTD ART followed by a selective residual ART. While the first
guard is paused, another connection drops the later ART. Its retained entry becomes RETIRED, the optional
read handle safely reports absence, and rejection of the broad ART reaches Sequential Scan with the correct
payload. Three payload-verified runs pass. Existing SQL coverage also verifies later-ART success when the
entry remains available. The probe-before-Fetch vacuum check passes all four single-/multi-filter and
completion/abandonment combinations with the current library. Controlled-pause and vacuum helpers remain
local. The additional local C++ case `ART read handles skip retired entries` retains an entry across a real
`DROP INDEX`, verifies a usable BOUND handle, and checks safe absence after `Retire` destroys the physical
index. Release and ASAN/UBSAN each pass its 11 assertions, without timing assumptions or production hooks.
The temporary case is removed from the repository after validation; its patch and logs remain local.
The SQL regression `test_art_codec_concurrent_insert.test` checks concurrent reopen/INSERT correctness;
it does not force overlap with metadata loading or independently detect restoration of the old list-lock scope.

### Current focused validation

The rebuilt Release passes 37,894 assertions in 28 cases covering ART scan, five related concurrent cases
and the upstream allocator-bitmap checkpoint regression, with the existing TPCH requirement skip. The new
concurrent-reopen test contributes 55 assertions. Across that test and codec admission, 30 applicable
storage/vector invocations execute successfully and 10 are skipped by their configuration's load policy.
The two encryption invocations stop before the test in initialization because this build lacks httpfs's
writable crypto provider; they are not passes. The static skill precheck checks whitespace, formatting and
Clang 20 Tidy. Complete local unit/config/sanitizer suites and exact-final-commit remote CI remain acceptance gaps.

The selected assertions + ASAN/UBSAN suite passes 17,889 assertions in 27 cases, with the TPCH skip and no
sanitizer diagnostics. The separately run existing `concurrent_writes_during_index_creation.test_slow`
still fails its count check: a table scan sees 20001 copies of key 1 while the subsequent indexed query
returns 98. A separately compiled same-object ASAN B control substitutes the new base `table_scan.cpp`,
records source/library hashes and commands, and is verified as actually loaded with `LD_PRELOAD` despite
the unittest's `DT_RPATH`. That B run fails at the same assertion, also returning 98 instead of 20001.
This establishes the failure without this patch on the rebased source; its underlying cause remains
unresolved and the extended sanitizer gate is not a pass. The existing test is unchanged. Original and
isolated logs are retained; LeakSanitizer was disabled for these focused ASAN/UBSAN runs.

The Avro compatibility patch is retained: the new main changes ART allocator bitmap tracking,
while the pinned Avro revision and its C++ compilation issue remain unchanged. The pre-rebase fork's Windows
core-extension job `113191204254` passes with that fix; this is historical Windows evidence. The current
`make extension-patch-check` passes after rebase. Final-commit fork CI is reported separately after push.

## Historical workload and measurement

The measurements before the same-base follow-up below are historical. Their runner used the requested
five seconds as the QPS denominator even when a final query finished later. Latency distributions and
CPU/query retain their original meanings; those historical QPS values cannot be corrected without an
actual end timestamp. The revised runner records both `requested_seconds` and actual `elapsed_s`.

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
about `0.065 ms` above B. The different source bases, other patch changes, and run noise prevent attributing
that difference to the guard.

The review follow-up adds two one-run targeted cases, both with `48-64` affinity: 128 candidates in a
100,000-row table. `medium_plain` stores the payload with Uncompressed segments and should keep the index
path; `medium_zstd` stores the payload with ZSTD and should exercise the guarded rejection path. Both queries
set `index_scan_max_count=2048` and `index_scan_percentage=0`, so the ZSTD cap is 64. Values are
p50 / CPU per query in milliseconds:

| Query | B baseline (Seq) | P patch without guard | G with guard |
| --- | ---: | ---: | ---: |
| 128 candidates, Uncompressed | 0.674 / 0.682 | 0.706 / 0.731 | 0.705 / 0.736 |
| 128 candidates, ZSTD | 1.238 / 1.311 | 7.716 / 7.793 | 1.292 / 1.360 |

The ZSTD case shows P's Fetch-first regression and G's rejection back to the sequential path. These are
historical path and direction checks from one five-second run on different source bases. They do not isolate
the guard's metadata-walk cost or establish a stable speedup.

B and P in this follow-up use the retained `8681dc5253` baseline/patch pair; G uses the current guarded
source after rebase to `e829ad1529`. G's thread sweep stayed stable on the fixed `48-64` CPU set:

| Query | threads=1 p50/CPU | threads=4 p50/CPU | threads=8 p50/CPU | threads=16 p50/CPU |
| --- | ---: | ---: | ---: | ---: |
| 128 candidates, Uncompressed | 0.705/0.736 | 0.700/0.808 | 0.697/0.807 | 0.692/0.807 |
| 128 candidates, ZSTD | 1.292/1.360 | 1.286/1.472 | 1.289/1.456 | 1.296/1.455 |

## Earlier same-base review follow-up (2026-10-08, before list-lock narrowing)

All controls use base `a770db1197ec428f11d7b9529c66389b4e40c87d`, GCC 11, and the same Release options
and link inputs. Only the table-function unity object differs: B uses the base `table_scan.cpp`, P disables
only codec admission in the corrected patch, and G uses the full corrected patch. S uses G with both index
thresholds set to zero on each connection. P and G retain the same vacuum-lock lifecycle and residual
filtering. The library actually loaded by every runner was checked; the source, compile/link commands,
library hashes, and equal input-database hash are retained in the external task's `controls/manifest.json`
and `native-manifest.json`.

The frozen data contains one million `orders` rows and 100,000 rows in each medium table. Both medium
lookups produce 128 candidates at row IDs 0–127 and return one 4-KiB payload. Storage metadata confirms
Uncompressed residual/payload segments in `medium_plain` and ZSTD segments in `medium_zstd`. P/G use Index
Scan for plain; P uses Index Scan for ZSTD, while G/S use Sequential Scan. S also uses Sequential Scan for
plain. This is concentrated row-ID locality, not a scattered-candidate result.

Each cell has one excluded warm-up interval followed by three five-second measurements, with variant order
rotated. Every worker passes the correctness gate before timing. Each live worker's affinity was checked
within `48-64`; this task's concurrent build processes were kept outside that CPU set. Wall time extends
through the last worker join, and process CPU covers the same completed queries. Below, each raw triplet
is followed by its arithmetic mean. Latency and CPU/query are milliseconds; QPS is queries/second.

| Query / control | p50: three runs → mean | CPU/query: three runs → mean | QPS: three runs → mean |
| --- | --- | --- | --- |
| Plain, P | 0.765 / 0.761 / 0.715 → 0.747 | 0.787 / 0.783 / 0.749 → 0.773 | 1276.5 / 1282.8 / 1353.9 → 1304.4 |
| Plain, G | 0.736 / 0.758 / 0.764 → 0.753 | 0.771 / 0.779 / 0.784 → 0.778 | 1322.9 / 1289.0 / 1281.2 → 1297.7 |
| Plain, S | 0.656 / 0.675 / 0.700 → 0.677 | 0.661 / 0.677 / 0.700 → 0.679 | 1514.8 / 1477.7 / 1429.6 → 1474.0 |
| ZSTD, P | 7.855 / 8.031 / 7.894 → 7.927 | 7.928 / 8.076 / 7.956 → 7.986 | 126.8 / 124.4 / 126.3 → 125.8 |
| ZSTD, G | 1.414 / 1.339 / 1.367 → 1.373 | 1.486 / 1.398 / 1.448 → 1.444 | 676.0 / 718.8 / 704.2 → 699.7 |
| ZSTD, S | 1.401 / 1.406 / 1.342 → 1.383 | 1.455 / 1.461 / 1.396 → 1.437 | 687.7 / 684.7 / 716.8 → 696.4 |

The ZSTD result supports the admission policy on this workload: `T_P / T_G = 5.77` for mean p50 and
`5.53` for CPU/query. G recovers the same-source Sequential Scan direction. Plain P/G differ by about
0.8% in mean p50 and 0.6% in CPU/query, with opposite directions between individual rounds; these runs do
not establish a stable metadata-walk cost or speedup. Plain Index Scan is itself slower than S here, so
this case establishes admission/path preservation rather than an index-performance win. Mean p95 for
plain P/G/S is 0.780/0.781/0.702 ms; for ZSTD it is 8.116/1.622/1.631 ms.

The single-filter control uses `SELECT count(*) FROM orders WHERE id = ?`, the same 30 point keys,
`threads=1`, and connections 1/8. Both B and G use Index Scan. The database allows persisted row-ID gaps,
so both builds take the shared vacuum lock even though `vacuum_rebuild_indexes=0`; the lock is already in
the base and is not an incremental change in this patch.

| Connections / control | p50: three runs → mean | CPU/query: three runs → mean | QPS: three runs → mean |
| --- | --- | --- | --- |
| 1, B | 0.569 / 0.592 / 0.591 → 0.584 | 0.574 / 0.594 / 0.594 → 0.587 | 1752.8 / 1692.4 / 1691.2 → 1712.1 |
| 1, G | 0.572 / 0.596 / 0.560 → 0.576 | 0.582 / 0.598 / 0.576 → 0.585 | 1743.9 / 1680.6 / 1779.9 → 1734.8 |
| 8, B | 0.578 / 0.582 / 0.586 → 0.582 | 0.580 / 0.610 / 0.585 → 0.592 | 13577.2 / 11203.7 / 13429.6 → 12736.9 |
| 8, G | 0.596 / 0.590 / 0.582 → 0.589 | 0.594 / 0.590 / 0.611 → 0.598 | 13195.9 / 13310.0 / 11694.7 → 12733.5 |

There is no consistent QPS loss across these controls; the eight-connection runs have a slower round in
each build. Their mean p95 is 0.767/0.764 ms for B/G. This does not isolate the cost of the inherited shared
lock. Peak process RSS across the measured cells is 59.0–77.7 MiB, including premeasurement gates/warm-up.
Raw JSON, CPU seconds, wall seconds, RSS, and analyzed plans remain in the external task's `native/run1`
through `native/run3` and `controls` directories. Warm-up and timing-smoke records are excluded from the
tables. A 1-ms target ZSTD smoke completed one query in 7.947 ms and reported 125.84 QPS, confirming that
the deadline tail is included in the denominator.

## Implementation boundary

The patch removes the `FilterCount() != 1` selection guard, but still uses only the first successful
single-column ART that passes the codec admission. It does not intersect indexes or add a cost model. The
candidate limit is applied before residual filtering and before codec admission.

After `DataTable::Fetch`, all single-column table filters are applied to the complete scan-input chunk
through the existing thread-local `ScanFilterInfo` and `ColumnSegment::FilterSelection`, sharing one
selection vector. That selection slices only the projected output, without slicing filter-only columns.
Multi-column expressions remain above the scan, and LocalStorage keeps its existing complete filtering
path. Single-filter ART admission
and residual-filter behavior are unchanged, without predicate-consumption tracking. The current upstream
base already holds a shared vacuum lock from ART probe through scan-state destruction when indexed vacuum
can move row IDs; this patch retains that lifecycle.

ART probes use an ordered entry snapshot outside the index-list mutex. Each read handle validates and
protects the physical index under its entry lock; the handle is released before codec admission can load
column metadata. Concurrently retired entries are skipped, and rejected candidates still try later ARTs.

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

## Earlier validation

- B/P independent builds and shared-library link passed.
- The guarded source Release G build and shared-library link passed.
- The B/P/G native checks were run with verified `48-64` affinity; all correctness gates passed and every
  JSON stayed within the 17-CPU bound.
- The codec-admission test passed 120 assertions; the previous three multi-filter tests passed 149 assertions.
- The previous Release and GCC 11 ASAN/UBSAN ART scan runs passed 635 assertions; one existing `require tpch` test was skipped.
- `git diff --check`, shell syntax checks, and clang-format checks passed.

Coverage includes residual predicates that all, some, or no candidates pass; duplicate and non-contiguous
IN values; NULL three-valued logic; filter-only and reordered projections; generated, rowid, nested and
multi-column residual expressions; empty multi-batches; candidate limits; later-index fallback; local
transaction rows; UPDATE/DELETE correctness; rollback/commit; snapshot readers; checkpoint/restart; and
compound-index fallback. Codec coverage includes actual DICT_FSST/ZSTD/Uncompressed persistence, equal and
over-limit boundaries, unreferenced wide columns, same-column mixed segments, and later-ART fallback. The
current UPDATE/DELETE planner path is still a sequential scan, so those cases verify write correctness
without claiming new ART-driven DML coverage.

The full sanitizer suite and full test suite remain unrun. The current concentrated/scattered layout check
is reported at the top of this document.

The earlier measurements show selective hot-key lookup gains and the 1,500-candidate regression of the
unguarded Fetch-first path. The current guard and locality comparison is at the top of this document.

## CI diagnosis and review validation (2026-10-08)

Main run `37483275778` tested `f324408ccce0b010caf59abab61d5413c1a34dad`. Its TSAN job reports an
index-list/LocalStorage lock-order inversion: scan initialization inside `IndexEntries()` acquires local
storage locks while the iterator still holds the index-list lock; insertion acquires them in the reverse
order. The corrected code leaves the loop before initializing the scan. The ART read handle being released
earlier did not release the iterator's list lock.

The same-base old-code library reproduces a 30-second timeout in `concurrent_checkpoint_insert`; the base
and corrected libraries complete in about 1.5 seconds. Library resolution was verified explicitly, because
the unittest binary's `DT_RPATH` otherwise overrides `LD_LIBRARY_PATH`. Thus the earlier blanket attribution
of these timeouts to baseline/environment failures was unsupported.

The rebuilt Release passes all 635 ART scan assertions and the four remote concurrent-test failures
(`concurrent_checkpoint_insert`, `concurrent_checkpoint_wal_index`, `concurrent_update_pk`, and
`test_art_concurrent_loop`), plus `Test table info api`. The ART scan suite and checkpoint-insert test also
pass in 15 relevant storage/execution configurations; all three vector modes pass. The encryption config
stops during initialization because this local build lacks httpfs's writable crypto provider, before the
test reaches the changed code. `make allunit` currently stops in its Python wrapper because the host has
Python 3.9 and the current wrapper requires newer union-type annotation support. These are local acceptance
gaps, not successful aggregate-config/full-suite results.

The rebuilt GCC 11 assertions + ASAN/UBSAN binary also passes the same 635 ART scan assertions, all four
concurrent tests, and `Test table info api`. This is focused sanitizer evidence; a complete sanitizer suite
and a new exact-commit TSAN run remain outstanding. The public native wrapper passes default and `seq`
smokes, including its affinity and CPU/wall checks, and the JSON summarizer accepts the new S/single rows.

A local-only C++ check stops immediately after real ART initialization and before any Fetch, then completes
CHECKPOINT on another connection. It verifies the reader's original key/residual/rowid, lock release on scan
completion or abandonment, and actual row-ID movement from 245760 to 122880 after release. All four
single-/multi-filter and completion/abandonment combinations pass with indexed vacuum enabled. Disabling
only the vacuum lock makes the check fail at the probe-before-Fetch boundary. This validates the retained
lifecycle separately from the SQLLogicTests; the C++ helper is not included in the commit.

The static skill precheck passes formatting, whitespace, and Clang 20 Tidy. The remote run also contains
a ZSTD giant-string SIGKILL and retry-recovered CSV/Windows file errors; their logs do not establish the
same cause as the lock inversion. The giant-string test passes all six assertions on both the same-base
and corrected local libraries, each using about 15.0 GiB peak RSS; this supports a resource-pressure
hypothesis but does not prove the remote kill's cause. The exact CSV test/config pair passes locally.
Windows file errors were not reproduced on this Linux host. A new exact-commit remote run is still
required to assess the complete CI result after publication.

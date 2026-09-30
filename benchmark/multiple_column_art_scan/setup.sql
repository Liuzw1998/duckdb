CREATE TABLE orders(payload VARCHAR, id BIGINT PRIMARY KEY, tenant_id INTEGER, status VARCHAR, amount INTEGER);
INSERT INTO orders
SELECT repeat('payload-' || (i % 37)::VARCHAR, 8), ((i * 48271) % 1000000)::BIGINT,
       (i % 100)::INTEGER, CASE WHEN i % 10 < 8 THEN 'ready' ELSE 'closed' END,
       (i % 1000)::INTEGER
FROM range(1000000) tbl(i);
CHECKPOINT;
CREATE TABLE candidate_orders AS
SELECT 42 AS lookup_key,
       'ready' AS status_all,
       CASE WHEN i < 10 THEN 'ready' ELSE 'reject' END AS status_few,
       CASE WHEN i = 0 THEN 'ready' ELSE 'reject' END AS status_one,
       repeat('n', 128) AS payload_narrow,
       repeat('w', 4096) AS payload_wide
FROM range(1500) tbl(i)
UNION ALL
SELECT i % 41, 'reject', 'reject', 'reject', repeat('n', 128), repeat('w', 4096)
FROM range(8500) tbl(i);
CREATE INDEX candidate_orders_lookup ON candidate_orders(lookup_key);
CHECKPOINT;

PRAGMA force_compression = 'uncompressed';
CREATE TABLE candidate_medium_plain AS
SELECT 42 AS lookup_key,
       CASE WHEN i = 1 THEN 'keep' ELSE 'reject' END AS residual,
       repeat('p', 4096) AS payload
FROM range(128) tbl(i)
UNION ALL
SELECT i % 41, 'reject', repeat('p', 128)
FROM range(99872) tbl(i);
CHECKPOINT;
CREATE INDEX candidate_medium_plain_lookup ON candidate_medium_plain(lookup_key);
CHECKPOINT;

PRAGMA force_compression = 'zstd';
CREATE TABLE candidate_medium_zstd AS
SELECT 42 AS lookup_key,
       CASE WHEN i = 1 THEN 'keep' ELSE 'reject' END AS residual,
       repeat('z', 4096) AS payload
FROM range(128) tbl(i)
UNION ALL
SELECT i % 41, 'reject', repeat('z', 128)
FROM range(99872) tbl(i);
CHECKPOINT;
CREATE INDEX candidate_medium_zstd_lookup ON candidate_medium_zstd(lookup_key);
CHECKPOINT;
PRAGMA force_compression = 'uncompressed';

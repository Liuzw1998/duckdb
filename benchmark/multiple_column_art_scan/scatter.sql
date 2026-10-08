-- Run on a copy of the setup database. Preserve every logical row and change only its physical order.
-- 71631 is the inverse of 48271 modulo 100000.
PRAGMA force_compression = 'uncompressed';
DROP INDEX candidate_medium_plain_lookup;
ALTER TABLE candidate_medium_plain RENAME TO locality_original_plain;
CREATE TABLE candidate_medium_plain AS
SELECT * FROM locality_original_plain ORDER BY (rowid * 71631) % 100000;
SELECT CASE WHEN count(*) = 0 THEN true ELSE error('plain row multiset differs') END
FROM (
    (SELECT * FROM locality_original_plain EXCEPT ALL SELECT * FROM candidate_medium_plain)
    UNION ALL
    (SELECT * FROM candidate_medium_plain EXCEPT ALL SELECT * FROM locality_original_plain)
) differences;
DROP TABLE locality_original_plain;
CREATE INDEX candidate_medium_plain_lookup ON candidate_medium_plain(lookup_key);
CHECKPOINT;

PRAGMA force_compression = 'zstd';
DROP INDEX candidate_medium_zstd_lookup;
ALTER TABLE candidate_medium_zstd RENAME TO locality_original_zstd;
CREATE TABLE candidate_medium_zstd AS
SELECT * FROM locality_original_zstd ORDER BY (rowid * 71631) % 100000;
SELECT CASE WHEN count(*) = 0 THEN true ELSE error('zstd row multiset differs') END
FROM (
    (SELECT * FROM locality_original_zstd EXCEPT ALL SELECT * FROM candidate_medium_zstd)
    UNION ALL
    (SELECT * FROM candidate_medium_zstd EXCEPT ALL SELECT * FROM locality_original_zstd)
) differences;
DROP TABLE locality_original_zstd;
CREATE INDEX candidate_medium_zstd_lookup ON candidate_medium_zstd(lookup_key);
CHECKPOINT;
PRAGMA force_compression = 'uncompressed';

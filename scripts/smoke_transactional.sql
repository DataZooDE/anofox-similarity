-- Transactional embeddings end to end, with the community anofox_forecast extension.
-- Run with a DuckDB CLI that has anofox_similarity loaded (CI: the stock CLI and the artifact):
--   duckdb -unsigned -cmd "LOAD 'anofox_similarity.duckdb_extension'" < scripts/smoke_transactional.sql
-- Every check raises an error (non-zero exit with -bail) when it fails; the last line prints OK.
.bail on
INSTALL anofox_forecast FROM community;
LOAD anofox_forecast;

-- Two years of weekly movements for six materials:
--   PUMP-A  delivered (601) every week, a seasonal pattern, a few 602 returns
--   PUMP-X  PUMP-A's pattern at ten times the volume
--   SEAL-B  issued to production (261) twice a week, every 10th issue reversed (262, signed negative)
--   MOTOR-C issued (261) every other week, growing
--   VALVE-D a new material: four weeks of history only
--   ODD-E   one posting: below min_observations, left out
CREATE OR REPLACE TABLE goods_movements AS
WITH w AS (SELECT i, DATE '2024-01-01' + INTERVAL (7 * i) DAY AS d FROM range(104) r(i))
SELECT 'PUMP-A' AS material_id, d::DATE AS movement_date,
       (10 + 5 * sin(i * 2 * pi() / 52))::DOUBLE AS quantity, '601' AS movement_type FROM w
UNION ALL SELECT 'PUMP-A', (d + INTERVAL 2 DAY)::DATE, 1.0, '602' FROM w WHERE i % 13 = 0
UNION ALL SELECT 'PUMP-X', d::DATE, (10 * (10 + 5 * sin(i * 2 * pi() / 52)))::DOUBLE, '601' FROM w
UNION ALL SELECT 'SEAL-B', d::DATE, (20 + i % 5)::DOUBLE, '261' FROM w
UNION ALL SELECT 'SEAL-B', (d + INTERVAL 3 DAY)::DATE, (15 + i % 3)::DOUBLE, '261' FROM w
UNION ALL SELECT 'SEAL-B', (d + INTERVAL 4 DAY)::DATE, -2.0, '262' FROM w WHERE i % 10 = 0
UNION ALL SELECT 'MOTOR-C', d::DATE, (1 + i / 10)::DOUBLE, '261' FROM w WHERE i % 2 = 0
UNION ALL SELECT 'VALVE-D', d::DATE, 3.0, '601' FROM w WHERE i >= 100
UNION ALL SELECT 'ODD-E', DATE '2025-12-01', 1.0, '601';

-- One refresh fills every row the embedding reads: positions 0-29 and 92-97
DELETE FROM transactional_embedding_statistics;
INSERT INTO transactional_embedding_statistics
SELECT *, now(), 1 FROM recompute_embedding_statistics(time_window_days := 730);
SELECT CASE WHEN count(*) = 36 AND count(*) FILTER (WHERE feature_index BETWEEN 92 AND 97) = 6
            AND bool_and(isfinite(mean_value) AND isfinite(std_value))
       THEN 'ok' ELSE error('statistics: ' || count(*) || ' rows, expected 36 finite (0-29, 92-97)') END AS check
FROM transactional_embedding_statistics;

CREATE OR REPLACE TABLE te AS SELECT * FROM compute_transactional_embeddings(time_window_days := 730);

-- 128 dimensions, no NaN, unit length, one row per qualifying material (ODD-E left out)
SELECT CASE WHEN count(*) = 5
            AND bool_and(len(transactional_embedding::FLOAT[]) = 128)
            AND NOT bool_or(list_bool_or(list_transform(transactional_embedding::FLOAT[], lambda x: NOT isfinite(x))))
            AND bool_and(abs(sqrt(list_dot_product(transactional_embedding::FLOAT[], transactional_embedding::FLOAT[])) - 1) < 1e-4)
       THEN 'ok' ELSE error('embeddings: ' || count(*) || ' rows, expected 5 finite unit vectors of 128') END AS check
FROM te;

-- the ERP positions carry the data: SEAL-B issues to production (92) and has reversals (93)
SELECT CASE WHEN a.e[93] > b.e[93] AND a.e[94] > 0 AND a.e[94] > c.e[94]
       THEN 'ok' ELSE error('ERP features: SEAL-B 92/93 = ' || a.e[93] || '/' || a.e[94]) END AS check
FROM (SELECT transactional_embedding::FLOAT[] AS e FROM te WHERE material_id = 'SEAL-B') a,
     (SELECT transactional_embedding::FLOAT[] AS e FROM te WHERE material_id = 'PUMP-A') b,
     (SELECT transactional_embedding::FLOAT[] AS e FROM te WHERE material_id = 'MOTOR-C') c;

-- a material against itself: cosine 1
SELECT CASE WHEN abs(array_cosine_similarity(transactional_embedding, transactional_embedding) - 1) < 1e-5
       THEN 'ok' ELSE error('self cosine') END AS check
FROM te WHERE material_id = 'PUMP-A';

SELECT 'OK transactional embeddings' AS result;

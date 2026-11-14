-- ============================================================================
-- AAS capability matching, step 3: hand the result to the supplier portal as AAS
-- ============================================================================
-- The portal speaks AAS v3. The RFQ goes back as IDTA-02020 Required
-- capabilities; each ranking as a CoverageResult submodel with the gap list.
-- Run: DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/aas_capability_matching/03_write_back_as_aas.sql
-- ============================================================================

.read examples/aas_capability_matching/sql/load.sql

CREATE OR REPLACE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'), labels := 'hard');

.print '>>> 1. Every tree as an AAS environment; read back, it is the same tree'
SELECT e.tree_id, length(e.aas_json) > 0 AS written,
       (SELECT count(*) FROM (SELECT n.node_id, n.parent_id, n.class_id FROM (SELECT unnest(aas_to_trees(e.aas_json, e.tree_id)) AS n)
                              EXCEPT ALL SELECT node_id, parent_id, class_id FROM tree_nodes WHERE tree_id = e.tree_id)) AS differences
FROM aas_export() e ORDER BY e.tree_id;
-- Expected:
-- | tree_id                                     | written | differences |
-- | RFQ-2026-117                                | true    | 0           |
-- | https://driventa.example/aas/ds-12          | true    | 0           |
-- | https://kessler-antriebe.example/aas/ksd-10 | true    | 0           |
-- | https://lenvaro.example/aas/lx-8            | true    | 0           |
-- | https://orbitec.example/aas/ox-6            | true    | 0           |

.print ''
.print '>>> 2. The gap list as an AAS CoverageResult submodel, one per offer'
SELECT o.offer, r.aas_json LIKE '%"idShort": "CoverageResult"%' AS is_submodel,
       (SELECT round(a.lo, 3) FROM (SELECT unnest(n.attributes) AS a FROM (SELECT unnest(aas_to_trees(r.aas_json)) AS n))
        WHERE a.attr_id = 'Coverage') AS coverage
FROM aas_coverage_result('trace') r JOIN offers o USING (candidate_id) ORDER BY coverage DESC;
-- Expected:
-- | offer          | is_submodel | coverage |
-- | Lenvaro LX-8   | true        | 1.0      |
-- | Orbitec OX-6   | true        | 0.75     |
-- | Kessler KSD-10 | true        | 0.625    |
-- | Driventa DS-12 | true        | 0.372    |

-- To write the files (the CSV writer with quoting off writes the JSON unchanged):
-- COPY (SELECT aas_json FROM aas_export(tree_ids := ['RFQ-2026-117']))
--   TO 'rfq_2026_117.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
-- The output validates against the AAS v3.2 JSON schema and loads into Eclipse BaSyx
-- (scripts/tests/test_aas_schema.py, scripts/aas_basyx_smoke.sh).

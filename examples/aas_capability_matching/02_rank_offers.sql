-- ============================================================================
-- AAS capability matching, step 2: rank the offers and read the gap list
-- ============================================================================
-- The capability classes come from a shared vocabulary, so labels are hard.
-- Properties are matched by their dictionary key (attr_key), whatever the
-- supplier called them: Driventa's peak current is another property than the
-- rated current, even though both IRDIs start with 0173-1#02-BAA12.
-- Run: DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/aas_capability_matching/02_rank_offers.sql
-- ============================================================================

.read examples/aas_capability_matching/sql/load.sql

.print '>>> 1. Ranking'
SELECT o.offer, round(c.coverage, 3) AS coverage, c.n_gaps
FROM tree_coverage('RFQ-2026-117', candidates := getvariable('offers'), labels := 'hard') c JOIN offers o USING (candidate_id)
ORDER BY c.rank;
-- Expected:
-- | offer          | coverage | n_gaps |
-- | Lenvaro LX-8   | 1.0      | 0      |
-- | Orbitec OX-6   | 0.75     | 1      |
-- | Kessler KSD-10 | 0.625    | 2      |
-- | Driventa DS-12 | 0.372    | 2      |

CREATE OR REPLACE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'), labels := 'hard');

.print ''
.print '>>> 2. The gap list: what each offer misses, and why'
SELECT o.offer, t.attr_id, t.req_value, t.prov_value, t.rule, round(t.attr_score, 2) AS score
FROM trace t JOIN offers o USING (candidate_id) WHERE t.attr_score < 0.99 ORDER BY o.offer, t.attr_id;
-- Expected:
-- | offer          | attr_id            | req_value                         | prov_value                  | rule         | score |
-- | Driventa DS-12 | AmbientTemperature | at_least 318.15 K                 | 313.15 K                    | at_least     | 0.0   |
-- | Driventa DS-12 | Protocol           | point EtherCAT (0173-1#07-AAZ001) | PROFINET (0173-1#07-AAZ002) | compat       | 0.0   |
-- | Driventa DS-12 | RatedCurrent       | at_least 7.6 A                    | 12 A                        | key_mismatch | 0.0   |
-- | Driventa DS-12 | SafeTorqueOff      | point true                        | false                       | compat       | 0.0   |
-- | Driventa DS-12 | SupplyVoltage      | within 380..480 V                 | 400..480 V                  | within       | 0.98  |
-- | Kessler KSD-10 | AmbientTemperature | at_least 318.15 K                 | 55                          | gate         | 0.0   |
-- | Kessler KSD-10 | CycleTime          | at_most 0.001 s                   | ?                           | missing      | 0.0   |
-- | Orbitec OX-6   | RatedCurrent       | at_least 7.6 A                    | 6 A                         | at_least     | 0.0   |
-- | Orbitec OX-6   | SupplyVoltage      | within 380..480 V                 | 200..240 V                  | within       | 0.0   |
-- Driventa's 12 A is a *peak* current (key_mismatch), Kessler's 55 has no unit
-- (gate) and its cycle time is "n/a" (missing): ask, do not reject.

.print ''
.print '>>> 3. Ground truth: every verdict agrees with the generator'
SELECT count(*) AS verdicts, count(*) FILTER ((t.attr_score >= 0.99) <> g.met) AS disagreements
FROM trace t JOIN offers o USING (candidate_id)
JOIN read_csv('examples/aas_capability_matching/data/gold_offer_verdicts.csv') g
  ON replace(g.offer, '_', ' ') = replace(replace(o.offer, '-', ' '), '_', ' ') AND g.requirement = t.attr_id;
-- Expected:
-- | verdicts | disagreements |
-- | 24       | 0             |

.print ''
.print '>>> 4. Ad hoc: which offers state a rated current of at least 7637,1 mA?'
SELECT o.offer, s.value, s.satisfied
FROM attrs_satisfying('https://api.eclass-cdp.com/0173-1-02-BAA120-009', parse_quantity('mindestens 7637,1 mA', 'de')) s
JOIN offers o ON o.candidate_id = s.tree_id
WHERE s.node_id LIKE 'CapabilityDescription%' ORDER BY s.score DESC, o.offer;
-- Expected:
-- | offer          | value | satisfied |
-- | Kessler KSD-10 | 10 A  | true      |
-- | Lenvaro LX-8   | 8 A   | true      |
-- | Orbitec OX-6   | 6 A   | false     |

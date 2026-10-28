# Round 4: planner as user

Scenario: `examples/planner_launch_phaseout/` (fictional VoltEdge). A demand planner needs a history
analog for the new `CHARGER-65W-EU-BLACK`, the predecessor of `CABLE-USB2-1M-V2`, the components a
phase-out strands, and a shortage alternative.

**Verdict:** The folder demonstrates the planning investigation convincingly. A customer would still
ask for a native active-week filter, a cut-over signal, stock availability, and a formal substitute
approval step.

## Setup

Every repro below starts from this block. Run it from the repository root in the DuckDB shell built
with the extension (`DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb`).

```sql
CREATE OR REPLACE TABLE raw_bom AS
SELECT * FROM read_csv('examples/planner_launch_phaseout/data/bom_edges.csv.gz');
CREATE OR REPLACE TABLE raw_movements AS
SELECT material_id, movement_date::DATE AS movement_date,
       movement_type::VARCHAR AS movement_type, quantity::DOUBLE AS quantity
FROM read_csv('examples/planner_launch_phaseout/data/movements.csv.gz');
-- 36 BOM positions are split over two rows: add them up
CREATE OR REPLACE TABLE bom_items AS
SELECT parent_id, child_id, sum(quantity)::DOUBLE AS quantity FROM raw_bom GROUP BY 1, 2;
CREATE OR REPLACE VIEW issues_only AS
SELECT material_id, movement_date, quantity FROM raw_movements
WHERE movement_type = '261' AND quantity > 0;
```

## Findings

| # | Finding | Severity |
|---|---------|----------|
| 1 | `cold_start_analogs` accepts sparse and returns-only history | Medium |
| 2 | `infer_predecessors` misses a clean cut-over | Medium |
| 3 | `infer_predecessors` looks at the top 100 BOM neighbours only | Low |
| 4 | Set similarity ignores BOM quantities | Medium |
| 5 | `find_similar_materials_jaccard` pads `k` with zero-similarity rows | Low |

### 1. Calendar span passes sparse and returns-only history (medium)

```sql
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('CHARGER-65W-EU-BLACK', 5, min_history_months := 12,
  min_similarity := 0.2, bom_table := 'bom_items', movements_table := 'raw_movements')
ORDER BY similarity DESC, material_id;
```

Actual:

| material_id | similarity | history_months |
|-------------|-----------:|---------------:|
| CHARGER-65W-EU-GREY | 0.875 | 34 |
| CHARGER-65W-EU-RETURN | 0.7647 | 27 |
| CHARGER-65W-EU-WHITE | 0.6667 | 35 |

GREY has two issues of one piece (2023-01-02 and 2025-11-17); RETURN has 120 postings, all returns
(type `262`, negative). Both pass `min_history_months := 12` and outrank WHITE, which has 156 active
issue weeks. Passing `issues_only` removes RETURN but keeps GREY.

Expected for forecast borrowing: only materials with dense consumption. Suggested fix: an
`active_weeks` output and threshold in `cold_start_analogs`, and documentation that
`movements_table` should hold consumption only. Bridge used in the example (01, steps 2 and 4):

```sql
CREATE OR REPLACE VIEW dense_issues AS
SELECT material_id, movement_date, quantity FROM issues_only
WHERE material_id IN (SELECT material_id FROM issues_only GROUP BY 1
                      HAVING count(DISTINCT date_trunc('week', movement_date)) >= 20);
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('CHARGER-65W-EU-BLACK', 5, min_history_months := 12,
  min_similarity := 0.2, bom_table := 'bom_items', movements_table := 'dense_issues');
-- CHARGER-65W-EU-WHITE | 0.6667 | 35
```

The threshold of 20 weeks is a planning policy, not a property of the data.

### 2. Clean cut-over yields no predecessor (medium)

```sql
SELECT count(*) AS inferred FROM infer_predecessors('CABLE-USB2-1M-V2',
  bom_table := 'bom_items', movements_table := 'issues_only');
-- 0
SELECT predecessor_id, round(confidence, 4) AS confidence, round(correlation, 4) AS correlation,
       round(similarity, 4) AS similarity, round(temporal_score, 4) AS temporal_score,
       predecessor_last_usage, successor_start, overlapping_weeks
FROM infer_predecessors('CABLE-USB2-1M-V2', bom_table := 'bom_items',
  movements_table := 'issues_only', min_confidence := 0);
```

Actual with `min_confidence := 0`:

| predecessor_id | confidence | correlation | similarity | temporal_score | predecessor_last_usage | successor_start | overlapping_weeks |
|----------------|-----------:|------------:|-----------:|---------------:|------------------------|-----------------|------------------:|
| CABLE-USB2-1M-V1 | 0.3556 | -0.0601 | 0.4286 | 0.6767 | 2024-06-24 | 2024-07-01 | 8 |

V1 is the only candidate and the right one, but its confidence 0.3556 is below the default
`min_confidence` of 0.5. The cause is the demand pattern: V1 stops in the week before V2 starts, at
about 100 a week each, so there is no overlap in which one falls while the other rises, and the
correlation is close to zero (-0.0601). Lowering `min_overlapping_weeks` to 0 does not change the
result.

Expected: V1 as a candidate at the default settings. Suggested fix: a cut-over signal (last issue of
the candidate shortly before the first issue of the successor) next to the anti-correlation term.
Bridge used in the example (02, step 3):

```sql
WITH launch AS (SELECT min(movement_date) AS first_issue FROM issues_only
                WHERE material_id = 'CABLE-USB2-1M-V2'),
  last_issues AS (SELECT material_id, max(movement_date) AS last_issue FROM issues_only GROUP BY 1)
SELECT s.material_id, round(s.similarity, 4) AS similarity, h.last_issue,
       date_diff('day', h.last_issue, l.first_issue) AS gap_days
FROM find_similar_materials_jaccard('CABLE-USB2-1M-V2', 20, min_similarity := 0.3,
  bom_table := 'bom_items') s
JOIN last_issues h USING (material_id) CROSS JOIN launch l
WHERE h.last_issue < l.first_issue AND h.last_issue >= l.first_issue - INTERVAL 12 WEEK;
-- CABLE-USB2-1M-V1 | 0.4286 | 2024-06-24 | 7
```

Catalogue approval is still required; neither result proves supersession.

### 3. Top-100 BOM-neighbour cap in `infer_predecessors` (low)

`infer_predecessors` scores only the 100 materials with the most similar BOMs to the successor
(documented in [`docs/guides/03-predecessors-and-cold-start.md`](../../guides/03-predecessors-and-cold-start.md),
section on predecessors). With 60 parents this fixture cannot hit the cap. In a real base with many
near-identical variants (colour, plug, packaging) more than 100 materials can share most components,
and the predecessor can be cut before its demand is looked at, with no message. The recipe in
finding 2 has no cap if its `k` is raised. Suggested fix: a `max_candidates` parameter, or a notice
when the cap removes candidates above `min_similarity`.

### 4. Similarity does not account for BOM quantities (medium)

Both searches over the whole BOM base, all 60 parents:

```sql
CREATE OR REPLACE TABLE component_features AS
SELECT parent_id AS entity_id, child_id AS feature_id, quantity AS weight FROM bom_items;
SELECT material_id, round(similarity, 4) AS jaccard, shared_components
FROM find_similar_materials_jaccard('CHARGER-65W-EU-BLACK', 5, min_similarity := 0.01,
  bom_table := 'bom_items');
SELECT entity_id, round(containment, 4) AS containment, shared_weight, query_weight
FROM find_covering_entities('CHARGER-65W-EU-BLACK', 5, feature_table := 'component_features');
```

Actual:

| material | jaccard | shared_components | containment | shared_weight / query_weight |
|----------|--------:|------------------:|------------:|-----------------------------:|
| CHARGER-65W-EU-GREY | 0.875 | 14 | 0.3889 | 7 / 18 |
| CHARGER-65W-EU-RETURN | 0.7647 | 13 | 0.7222 | 13 / 18 |
| CHARGER-65W-EU-WHITE | 0.6667 | 12 | 0.7778 | 14 / 18 |

Only the three other chargers share a component with BLACK, so the earlier restriction to
`CHARGER-65W-EU-%` did not change the ranking. Set Jaccard ranks GREY first, although GREY holds 0.5
of each component; quantity containment ranks WHITE first. Expected for a shortage: rank by required
quantities and list the gaps (WHITE lacks 3 components, 4 of 18 units). Suggested fix: a
`weight_col` option or quantity mode in the BOM search, and a per-component gap list. Note that
`find_covering_entities` can read `bom_items` directly with
`entity_col := 'parent_id', feature_col := 'child_id', weight_col := 'quantity'`, so the renamed
feature table is a convenience, not a requirement. A score of 0.7778 does not authorise substitution.

### 5. Zero-similarity rows fill `k` (low)

```sql
SELECT material_id, round(similarity, 4) AS jaccard
FROM find_similar_materials_jaccard('CHARGER-65W-EU-BLACK', 5, bom_table := 'bom_items');
```

Actual: the three chargers, then `CABLE-USB2-1M-V1` and `CABLE-USB2-1M-V2` at 0.0, which share no
component with BLACK. This is documented (`min_similarity` defaults to 0.0), but a planner reading
"top 5 similar materials" takes the cables as weak matches. Suggested fix: a default
`min_similarity` above 0, or exclude materials with no shared component.

## What a customer would still ask

- **Stock and open orders.** Coverage ranks BOMs; the planner needs unrestricted stock of the
  substitute's components, open purchase and production orders, and the quantities they free up.
- **Effectivity dates.** The cut-over is inferred from movements. Real BOMs carry valid-from and
  valid-to dates and change numbers; the search should respect them and the predecessor should be
  checked against them.
- **An approval workflow.** A predecessor or substitute candidate needs a reviewer, a status
  (proposed, approved, rejected) and an audit trail before it reaches the material master or MRP.
- **Density in the API.** Active weeks and a movement-type filter in `cold_start_analogs`, so the
  bridge views are not needed (finding 1).
- **Run-down of stranded stock.** The six V1-only components (`VE-COMP-039` to `-044`) need a
  quantity on hand and a consumption plan, not only a where-used count.

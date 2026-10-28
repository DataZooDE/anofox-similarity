# Predecessors and Cold-Start Examples

> **A new material rarely starts from nothing: it replaces an old one, or resembles one.**

Runnable SQL examples for finding which material a new one replaced (predecessor inference) and
which existing materials can lend their demand history to a new one (cold-start analogs).

## Functions

| Function | Description |
|----------|-------------|
| `infer_predecessors(material_id, ...)` | Similar BOM + falling demand of the old material while the new one rises |
| `cold_start_analogs(material_id, k, ...)` | Most similar materials that have enough consumption history |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`infer_predecessors.sql`](infer_predecessors.sql) | Anti-correlated replacement, direction check, thresholds | Inline + `test/data/synthetic` (scenario S7) |
| [`cold_start_analogs.sql`](cold_start_analogs.sql) | Analogs with and without a history requirement | Inline + `test/data/synthetic` (product families) |

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/predecessors_cold_start/infer_predecessors.sql
./build/release/duckdb < examples/predecessors_cold_start/cold_start_analogs.sql
```

---

## Usage

```sql
-- Which material did NEW-PUMP replace?
SELECT predecessor_id, confidence, correlation, similarity, overlapping_weeks
FROM infer_predecessors('NEW-PUMP', min_similarity := 0.5, min_confidence := 0.3);

-- Which materials with at least a year of history can stand in for NEW-PUMP?
SELECT material_id, similarity, history_months
FROM cold_start_analogs('NEW-PUMP', 5, min_history_months := 12);
```

## Input Tables

| Table | Columns |
|-------|---------|
| `bom_items` (`bom_table`) | `parent_id, child_id` |
| `goods_movements` (`movements_table`) | `material_id, movement_date, quantity` |

## Parameters

| Parameter | Default | Function | Description |
|-----------|---------|----------|-------------|
| `lookback_months` | 24 | `infer_predecessors` | History window before the successor started |
| `min_similarity` | 0.3 / 0.0 | both | Minimum Jaccard similarity of the BOMs |
| `min_confidence` | 0.5 | `infer_predecessors` | 0.4 × anti-correlation + 0.3 × similarity + 0.3 × temporal score |
| `lag_weeks` | 8 | `infer_predecessors` | Shift between the predecessor's decline and the successor's rise |
| `min_overlapping_weeks` | 8 | `infer_predecessors` | Minimum aligned weekly buckets |
| `min_history_months` | 0 | `cold_start_analogs` | Minimum months between first and last movement |

## Key Concepts

- **Dense weekly data.** Predecessor detection correlates weekly sums. A handful of movements
  months apart returns no rows; lower `min_overlapping_weeks` for sparse data.
- **Direction.** A predecessor was in use before the successor started; the newer material of
  a pair is never reported as the predecessor of the older one (`infer_predecessors.sql`,
  section 2).
- **Positional arguments.** The material id (and `k` for `cold_start_analogs`) are positional.

# Predecessors and Cold Start

> Use consumption history to find the material a new one replaces, and analogs to forecast it.

**Use this guide to:**
- Detect predecessor-successor pairs with `infer_predecessors`
- Find analogs with history for a new product with `cold_start_analogs`
- Read the output columns and decide which rows to trust
- Avoid the most common cause of empty results: sparse movement data

The examples use the synthetic dataset in `test/data/synthetic`. Run them from the repository root.

## Load the Data

Both functions need a BOM table and a movements table.

```sql
CREATE TABLE bom_items AS
SELECT parent_id, child_id, quantity FROM read_csv_auto('test/data/synthetic/bom_items.csv.gz');

CREATE TABLE goods_movements AS
SELECT material_id, movement_date, quantity
FROM read_csv_auto('test/data/synthetic/goods_movements.csv.gz');
```

`goods_movements` needs the columns `material_id`, `movement_date` (a DATE) and `quantity`. Other
columns are ignored. The data has 3 991 movements of 41 materials, about one per week.

## How Predecessor Detection Works

A successor replaces its predecessor. Three signals point to such a pair:

1. **Similar BOMs.** Jaccard similarity of the components, at least `min_similarity` (default 0.3).
2. **Anti-correlated consumption.** The weekly consumption of the old material falls while the new
   one rises. The series are aligned in weekly buckets and shifted by `lag_weeks` (default 8).
   At least `min_overlapping_weeks` (default 8) buckets must overlap.
3. **Succession in time.** The old material's usage ends around the time the new one starts.

The confidence combines the three. Rows below `min_confidence` (default 0.5) are dropped.

## Find a Predecessor

```sql
SELECT predecessor_id, round(confidence, 3) AS confidence, round(correlation, 3) AS correlation,
       round(similarity, 4) AS similarity, round(temporal_score, 3) AS temporal_score,
       predecessor_last_usage, successor_start, overlapping_weeks
FROM infer_predecessors('S7-PRED-NEW');
```

| predecessor_id | confidence | correlation | similarity | temporal_score | predecessor_last_usage | successor_start | overlapping_weeks |
|----------------|-----------:|------------:|-----------:|---------------:|------------------------|-----------------|------------------:|
| S7-PRED-OLD    | 0.837      | -0.983      | 0.6667     | 0.813          | 2023-03-19             | 2022-11-27      | 25                |

The material id is the first, positional argument. The tuning arguments are named.

### Reading the Output

| Column | Meaning | What to look for |
|---|---|---|
| `confidence` | Combined score in [0, 1] | Rank by it. Review pairs below about 0.7. |
| `correlation` | Correlation of the aligned weekly series | Negative: one falls as the other rises. |
| `similarity` | Jaccard similarity of the BOMs | Low values mean a different product. |
| `temporal_score` | How well the time order fits a succession | Low values: the time order does not fit. |
| `predecessor_first_usage`, `predecessor_last_usage` | Usage window of the candidate | The window should start before `successor_start`. |
| `successor_start` | First usage of the queried material | |
| `overlapping_weeks` | Aligned weekly buckets used for the correlation | Few weeks make the correlation unstable. |

### Check the Time Order

`S12-GEN-00` … `S12-GEN-05` form a generation chain. Each generation replaces the one before.
The next generation, `S12-GEN-04`, is just as similar to `S12-GEN-03` as `S12-GEN-02` is, and its
consumption is anti-correlated as well. Only the time order tells them apart: a predecessor was in
use before the successor started.

```sql
SELECT predecessor_id, round(confidence, 3) AS confidence, round(temporal_score, 3) AS temporal_score,
       predecessor_first_usage, successor_start
FROM infer_predecessors('S12-GEN-03');
```

| predecessor_id | confidence | temporal_score | predecessor_first_usage | successor_start |
|----------------|-----------:|---------------:|-------------------------|-----------------|
| S12-GEN-02     | 0.704      | 0.723          | 2020-01-08              | 2021-01-08      |

`S12-GEN-04` started after `S12-GEN-03`, so it is not listed: only materials in use before the
successor started can be its predecessor.

### Tuning

| Parameter | Default | Raise it to | Lower it to |
|---|---|---|---|
| `min_similarity` | 0.3 | demand closer BOMs | accept redesigns with many new parts |
| `min_confidence` | 0.5 | keep only clear cases | see weak candidates for review |
| `min_overlapping_weeks` | 8 | demand longer overlap | work with shorter overlap (less reliable) |
| `lag_weeks` | 8 | allow a longer ramp-up | match fast changeovers |
| `lookback_months` | 24 | search older materials | search recent ones only |

A stricter setting can return nothing. `S7-PRED-OLD` overlaps 25 weeks:

```sql
SELECT count(*) AS n FROM infer_predecessors('S7-PRED-NEW', min_overlapping_weeks := 30);
```

| n |
|--:|
| 0 |

## Pitfall: Sparse Data Returns Nothing

The correlation needs dense, overlapping weekly history. A few movements months apart give no
result, also with relaxed thresholds:

```sql
CREATE TABLE sparse_movements AS SELECT * FROM (VALUES
  ('S7-PRED-OLD', DATE '2022-01-10', 120.0), ('S7-PRED-OLD', DATE '2022-06-13', 80.0),
  ('S7-PRED-NEW', DATE '2022-09-12',  30.0), ('S7-PRED-NEW', DATE '2023-01-09', 90.0))
  t(material_id, movement_date, quantity);

SELECT count(*) AS n
FROM infer_predecessors('S7-PRED-NEW', movements_table := 'sparse_movements', min_overlapping_weeks := 1);
```

| n |
|--:|
| 0 |

An empty result does not mean "no predecessor". Check the data first:

```sql
SELECT material_id, count(*) AS movements,
       count(DISTINCT date_trunc('week', movement_date)) AS weeks,
       min(movement_date) AS first, max(movement_date) AS last
FROM goods_movements WHERE material_id IN ('S7-PRED-OLD', 'S7-PRED-NEW')
GROUP BY material_id ORDER BY material_id;
```

| material_id | movements | weeks | first      | last       |
|-------------|----------:|------:|------------|------------|
| S7-PRED-NEW | 63        | 63    | 2022-11-27 | 2024-02-04 |
| S7-PRED-OLD | 116       | 116   | 2021-01-03 | 2023-03-19 |

You need many weeks with movements on both sides, and a period in which both materials move. If
your movements table mixes receipts and issues, pass only the issues (consumption), for example
through a view.

## Cold Start: Analogs for a New Product

A new product has no history. `cold_start_analogs` returns similar materials that do have history.
Their consumption is a starting point for the forecast.

Add a new product that reuses most parts of `FAMILY-00-VAR-1`:

```sql
INSERT INTO bom_items
SELECT 'FAMILY-00-NEXT', child_id, quantity FROM bom_items WHERE parent_id = 'FAMILY-00-VAR-1'
UNION ALL SELECT 'FAMILY-00-NEXT', 'FAMILY-00-NEXT-NEW-000', 1.0;

SELECT material_id, round(similarity, 4) AS similarity, shared_components,
       history_months, first_usage, last_usage
FROM cold_start_analogs('FAMILY-00-NEXT', 3, min_history_months := 12, min_similarity := 0.3);
```

| material_id     | similarity | shared_components | history_months | first_usage | last_usage |
|-----------------|-----------:|------------------:|---------------:|-------------|------------|
| FAMILY-00-VAR-1 | 0.9091     | 10                | 17             | 2022-07-01  | 2023-12-22 |
| FAMILY-00-BASE  | 0.75       | 9                 | 47             | 2020-01-01  | 2023-12-20 |
| FAMILY-00-VAR-2 | 0.5        | 7                 | 23             | 2022-01-01  | 2023-12-23 |

Only materials with movement history are returned. `k` (here 3) is positional.

How to use the result:

- Weight the analogs' demand by `similarity`, or take the closest one.
- Prefer analogs with a long `history_months` when you need seasonality.
- Check `last_usage`. An analog that stopped long ago describes an old market.

If no material has enough history, the result is empty:

```sql
SELECT count(*) AS n FROM cold_start_analogs('FAMILY-00-NEXT', 3, min_history_months := 100);
```

| n |
|--:|
| 0 |

## Pitfalls

| Pitfall | Solution |
|---|---|
| Empty predecessor result | Check weekly density and overlap. Lower `min_overlapping_weeks` only with care. |
| `query_material_id :=` or `k :=` raises an error | The material id and `k` are positional. |
| Receipts and issues in one table | Filter to consumption before you call the functions. |
| Missing column error | The movements table needs `material_id`, `movement_date`, `quantity`. |

## Next Steps

- [Material similarity](02-material-similarity.md): Jaccard and WL search
- [Evaluation and tuning](07-evaluation-and-tuning.md): measure results against known pairs
- API: [Material search](../api/02-material-search.md)
- [Examples](../../examples/predecessors_cold_start)

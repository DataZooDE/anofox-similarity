# Predecessors and Cold Start

> How `infer_predecessors` decides that one material replaced another, and how `cold_start_analogs`
> picks materials whose history can stand in for a new one. For the calls, see
> [guide 03](../guides/03-predecessors-and-cold-start.md) and [api/02](../api/02-material-search.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Two Questions, One Kind of Evidence

A new material either replaces an old one (a redesign, a new supplier part, a cost-down version)
or it is new to the range. Planning needs to know which:

| | Predecessor | Cold-start analog |
|---|---|---|
| Question | Which material did this one replace? | Which materials are built like this one and have history? |
| Evidence | similar BOMs, consumption that hands over, time order | similar BOMs, enough history |
| Use | carry history over, phase out stock, link forecasts | a starting point for the forecast of a product without history |
| Function | `infer_predecessors` | `cold_start_analogs` |

Both start from **BOM similarity**: the Jaccard similarity of the component sets (shared
components / all components of the two). Products built from the same parts serve the same
purpose and see similar demand. The measure is cheap and explainable, but it ignores structure
and quantities; see [similarity scalars](../api/01-similarity-scalars.md) for the alternatives.

## Predecessor: Three Signals

A successor replaces its predecessor. Three signals point to such a pair:

1. **Similar BOMs.** Jaccard similarity at least `min_similarity` (default 0.3). Only the 100 most
   similar materials are considered.
2. **Consumption that hands over.** The weekly consumption of the old material falls while the
   new one rises. Both series are summed per week. The old one is shifted by `lag_weeks` (default
   8): week *t* of the new material is compared with week *t − 8* of the old one, because a
   ramp-up usually follows the decision to stop the old part. At least `min_overlapping_weeks`
   (default 8) aligned weeks must exist, and their correlation must be negative.
3. **Time order.** The old material was in use before the new one started, and its use ends
   around that start. The temporal score is 1.0 when the old material ends at the successor's
   start and falls to 0.7 six months later; ending up to three months before the start scores
   0.7 down to 0.4; ending later than six months after scores 0.3; anything else excludes the pair.

The confidence weights the three:

```
confidence = 0.4 · max(0, −correlation) + 0.3 · similarity + 0.3 · temporal_score
```

Pairs below `min_confidence` (default 0.5) are dropped. A worked example: a valve whose new
version shares three of five components and takes over over 16 weeks:

```sql
CREATE OR REPLACE TABLE bom_items AS SELECT * FROM (VALUES
  ('VALVE-OLD', 'BODY-1'), ('VALVE-OLD', 'SEAT-1'), ('VALVE-OLD', 'STEM-1'), ('VALVE-OLD', 'SEAL-A'),
  ('VALVE-NEW', 'BODY-1'), ('VALVE-NEW', 'SEAT-1'), ('VALVE-NEW', 'STEM-1'), ('VALVE-NEW', 'SEAL-B'))
  t(parent_id, child_id);
-- VALVE-OLD runs at 100 a week, then ramps down; VALVE-NEW ramps up from week 20 and takes over
CREATE OR REPLACE TABLE goods_movements AS
SELECT 'VALVE-OLD' AS material_id, DATE '2023-01-02' + 7 * w::INTEGER AS movement_date,
       CASE WHEN w < 20 THEN 100.0 ELSE 100.0 - 6 * (w - 19) END AS quantity
FROM range(0, 36) t(w)
UNION ALL
SELECT 'VALVE-NEW', DATE '2023-01-02' + 7 * w::INTEGER, LEAST(100.0, 6.0 * (w - 19))
FROM range(20, 60) t(w);

SELECT predecessor_id, confidence, round(correlation, 3) AS correlation, similarity,
       temporal_score, successor_start, predecessor_last_usage, overlapping_weeks
FROM infer_predecessors('VALVE-NEW');
-- => VALVE-OLD | 0.7836 | -0.89 | 0.6 | 0.825 | 2023-05-22 | 2023-09-04 | 24
```

The numbers: correlation −0.89 gives 0.356, similarity 3/5 gives 0.18, and the old valve ended 105
days after the new one started (1 − 105/180 · 0.3 = 0.825) gives 0.2475; with the unrounded correlation the sum is 0.7836.

## Why the Lag Matters, and What It Cannot See

The lag decides which weeks are compared. With `lag_weeks := 0` the same pair overlaps only in the
16 ramp weeks, where the two series are exact mirrors (correlation −1.0); with the default 8 the
comparison also covers weeks before the ramp, so the correlation is weaker but rests on 24 weeks.
Set the lag to the ramp-up time your products typically need.

Two patterns defeat the correlation by design:

- **A clean cut-over.** The old material stops from one week to the next and the new one starts at
  the same level. There is no ramp in which one falls while the other rises, so the correlation is
  noise and the pair is dropped. Guide 03 shows a time-window recipe for this case.
- **Sparse history.** A few movements months apart give too few aligned weeks. An empty result then
  means "no evidence", not "no predecessor".

The signal also depends on what counts as consumption: goods receipts and stock corrections in the
movements make the demand curve wrong (see [ERP BOM models](erp-bom-models.md#consumption-from-mseg)).

## Cold Start: Analogs With History

A new product has no history, so its forecast borrows from products built like it.
`cold_start_analogs` ranks materials by BOM similarity and keeps those with movements, reporting the
span of their history. A third valve that shares two parts with each:

```sql
INSERT INTO bom_items VALUES ('VALVE-NEXT', 'BODY-1'), ('VALVE-NEXT', 'SEAT-1'),
                             ('VALVE-NEXT', 'STEM-2'), ('VALVE-NEXT', 'SEAL-B');
SELECT material_id, round(similarity, 3) AS similarity, shared_components, history_months
FROM cold_start_analogs('VALVE-NEXT', 3);
-- => VALVE-NEW | 0.6 | 3 | 9
-- => VALVE-OLD | 0.333 | 2 | 8
```

`history_months` is the calendar span from the first to the last movement, not the number of
months with movements: two movements two years apart count as 24. Check the density before you
forecast from an analog, and check `last_usage`: an analog that stopped long ago describes an old
market. Weight the analogs' demand by `similarity`, or take the closest one.

## Why These Defaults

| Default | Why |
|---|---|
| `min_similarity` 0.3 | A successor reuses a good part of the old BOM; below that it is usually a different product |
| `lag_weeks` 8 | About two months between the decision and the ramp-up of a replacement |
| `min_overlapping_weeks` 8 | A correlation over fewer weekly points is unstable |
| `min_confidence` 0.5 | Similarity and time order alone reach at most 0.6, so weak demand evidence needs strong BOM and time evidence |
| Weights 0.4 / 0.3 / 0.3 | The hand-over in demand weighs most; BOM similarity and time order confirm it |
| 100 most similar materials | Bounds the correlation work per query; a predecessor among many near-identical variants can fall outside |
| `min_history_months` 0 (cold start) | Return every analog with movements; raise it when you need seasonality |

The design history (use cases, the original algorithm) is in the
[concept memo](../background/CONCEPT.md); it records how the method was chosen, not the current
contract.

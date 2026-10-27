# anofox-similarity Examples

Runnable, self-contained SQL examples for the anofox-similarity DuckDB extension. Every file
loads its own data (files tracked in `test/data/` or small tables built inline), explains each
step in comments and states the expected output next to each query.

## Topics

| Folder | What it shows | Main functions |
|--------|---------------|----------------|
| [`material_similarity/`](material_similarity/) | Compare BOMs by shared parts and by structure; explode BOMs | `jaccard_similarity`, `find_similar_materials_jaccard`, `wl_kernel_similarity`, `find_similar_materials_wl_kernel`, `bom_explosion_*`, `bom_where_used`, `bom_common_components` |
| [`predecessors_cold_start/`](predecessors_cold_start/) | Which material did a new one replace; which materials can lend it a history | `infer_predecessors`, `cold_start_analogs` |
| [`coverage_matching/`](coverage_matching/) | Does an offer give what a request asks for: ranking and gap lists | `find_covering_entities`, `find_covering_entities_for`, `tree_coverage`, `tree_coverage_trace` |
| [`quantities/`](quantities/) | Parse specification values (EN/DE) and compare them with a direction | `quantity_units`, `parse_quantity`, `normalize_quantity`, `value_satisfies`, `compat_score` |
| [`classification_review/`](classification_review/) | Map items onto a catalogue with your own vectors; review only the doubtful ones | `classify_to_catalogue`, `vocabulary_review_queue`, `apply_vocabulary_review`, `uncertain_decisions` |
| [`evaluation/`](evaluation/) | Score a pipeline against ground truth; tune the kernel weights | `eval_ranking`, `eval_calibration`, `threshold_at_precision`, `bootstrap_ci`, `fit_coverage_weights` |
| [`dedup_and_lsh/`](dedup_and_lsh/) | Cluster duplicate records; MinHash/LSH candidate search | `cluster_matches`, `minhash_index`, `minhash_bands`, `find_covering_entities_lsh` |

A suggested reading order for coverage matching: `quantities` → `coverage_matching` →
`classification_review` → `evaluation`.

## Running the Examples

Build the extension first (`make release`). The shell in `build/release/` has the extension
linked in, so no `LOAD` is needed. Run the files from the repository root, because the data
paths are relative to it:

```bash
# One file
./build/release/duckdb < examples/coverage_matching/tree_coverage_mini.sql

# Every file, failing on any error
scripts/run_examples.sh
```

Set `DATAZOO_DISABLE_TELEMETRY=1` to switch off the load-time telemetry event.

With an installed DuckDB, load the extension at the top of a session instead:

```sql
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
```

## Data

| Data set | Path | Used by |
|----------|------|---------|
| AdventureWorks bicycle BOMs | `test/data/adventureworks/` | material_similarity |
| Synthetic BOMs and weekly goods movements | `test/data/synthetic/` | predecessors_cold_start |
| Seating coverage benchmark (requests, programmes, vectors, customers) | `test/data/coverage/` | coverage_matching, quantities, classification_review, evaluation, dedup_and_lsh |
| Hand-made coverage fixture | `test/data/coverage/mini/` | coverage_matching |

The coverage benchmark is described in
[`test/data/coverage/README.md`](../test/data/coverage/README.md). Files whose names start with
`gold_` are ground truth: the examples read them only to score results or, where noted, as a
stand-in for a finished human review.

## Notes

- The expected values in the comments come from the current build. `bootstrap_ci` intervals use
  DuckDB's hash and can differ slightly between DuckDB versions.
- On DuckDB 1.5.6, named arguments of **scalar** functions bind by position, so pass them in the
  documented order. Table functions bind by name on all supported versions.
- Leading ids and `k` of the search table functions are positional
  (`find_similar_materials_jaccard('AW-749', 5)`).

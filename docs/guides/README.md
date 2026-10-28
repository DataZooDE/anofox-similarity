# Guides

> Task-based guides for anofox-similarity. Each one has runnable SQL and the output to expect.

Run the examples from the repository root with the built shell, `./build/release/duckdb`. They use
only the data in `test/data` or small inline tables.

| # | Guide | You learn to |
|---|---|---|
| 01 | [Getting started](01-getting-started.md) | Build and load the extension, run a first query, learn the input tables and how arguments bind |
| 02 | [Material similarity](02-material-similarity.md) | Find similar products by BOM with Jaccard or the WL kernel, and traverse BOMs |
| 03 | [Predecessors and cold start](03-predecessors-and-cold-start.md) | Detect predecessor-successor pairs and find analogs with history for new products |
| 04 | [Coverage matching](04-coverage-matching.md) | Rank past solutions by how much of a request they cover, and list the gaps |
| 05 | [Quantities and specifications](05-quantities-and-specs.md) | Parse EN/DE limits with units and check values with a direction and a tolerance |
| 06 | [Classification and review](06-classification-and-review.md) | Classify items with your own vectors and review the uncertain ones |
| 07 | [Evaluation and tuning](07-evaluation-and-tuning.md) | Measure ranking and calibration, set an auto-accept threshold, fit weights |
| 08 | [Scale and deduplication](08-scale-and-dedup.md) | Use MinHash/LSH for large archives and cluster duplicate records |
| 09 | [Nightly pipelines](09-nightly-pipelines.md) | Run it as a scheduled job on a database file: re-runs, data gates, rebuilds, readers |

## Where to Start

| Your task | Read |
|---|---|
| "Which existing product is like this one?" | 01 → 02 |
| "What did this new material replace, and how do I forecast it?" | 01 → 02 → 03 |
| "Which past solution covers this customer specification?" | 01 → 04 → 05 → 06 |
| "Can we trust the matches?" | 07 |
| "The archive has a million entries" or "the customer master has duplicates" | 08 |
| "This has to run every night" | 09 |

## Tutorials

End-to-end walkthroughs on real ERP data (AdventureWorks and SAP) are in [tutorials/](tutorials/).

## See Also

- API reference: [conventions](../api/00-conventions.md) and one page per function group in [../api/](../api/)
- Runnable scripts: [../../examples/](../../examples/)

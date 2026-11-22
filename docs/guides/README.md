# Guides

> Task-based guides for anofox-similarity. Each one has runnable SQL and the output to expect.

Run the examples from the repository root with the built shell, `./build/release/duckdb`. They use
only the data in `test/data`, `examples/*/data` or small inline tables. `make check-docs` runs every
guide top down.

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
| 10 | [The DataZoo AAS profile](10-aas-profile.md) | Advanced: express configuration variants and additive counts in an Asset Administration Shell |
| 11 | [AAS: first match](11-aas-first-match.md) | Audit, load and rank AAS offers against an AAS request, read the gaps, write the result back |
| 12 | [From an ERP extract](12-from-erp-extract.md) | Export the right SAP or Dynamics 365 tables and turn them into BOM edges, texts, consumption and a first similarity search |
| 13 | [From a tender to trees](13-tender-to-trees.md) | Turn an extracted tender into trees the kernel can score: JSON, `attr_key`, classes, the clause sidecar |

## Where to Start

| Your task | Read |
|---|---|
| "Which existing product is like this one?" | 01 → 12 → 02 |
| "What did this new material replace, and how do I forecast it?" | 01 → 12 → 02 → 03 |
| "Which past solution covers this customer specification?" | 01 → 04 → 13 → 06 → 05 |
| "Can we trust the matches?" | 07 |
| "The archive has a million entries" or "the customer master has duplicates" | 08 |
| "This has to run every night" | 09 |
| "Requests and offers arrive as Asset Administration Shells" | 11 → 10 ([AAS API](../api/17-aas.md) for the mapping) |

## Using an AI Assistant

The same material, condensed for Claude Code, ships as a plugin with six skills that load when your
request matches. See [Claude Code skills](../../README.md#-claude-code-skills-ai-pair-programming)
for the install line.

## Tutorials

End-to-end walkthroughs on real ERP data (AdventureWorks and SAP) are in [tutorials/](tutorials/).

## See Also

- How it works and why: [concepts](../concepts/README.md)

- API reference: [conventions](../api/00-conventions.md) and one page per function group in [../api/](../api/)
- Runnable scripts: [../../examples/](../../examples/)

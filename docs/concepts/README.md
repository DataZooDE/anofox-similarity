# Concepts

> How and why anofox-similarity works: the models behind the functions. Read these when a result
> surprises you or when you tune a parameter; for a first result, start with the
> [guides](../guides/README.md).

The documentation has two layers. The **usage layer** (README, [guides](../guides/README.md),
[examples](../../examples/README.md), [API reference](../API_REFERENCE.md) and the Claude Code
skills) says what to call and what comes back. The **concepts layer** (this folder) says what the
numbers mean, which rules produce them, and why the defaults are what they are. Each concepts
page links back to the guide and the API page that use it.

| Page | Explains | Used by |
|---|---|---|
| [Coverage semantics](coverage-semantics.md) | What coverage measures, the two stages, the kernel formula, assignment, alternatives, names and keys, class hierarchy, gaps | [guide 04](../guides/04-coverage-matching.md), [api/13](../api/13-coverage-kernel.md) |
| [Quantities and bounds](quantities-and-bounds.md) | Base units and dimensions, bound words, tolerances and spans, the scoring rules per bound, temperature | [guide 05](../guides/05-quantities-and-specs.md), [api/11](../api/11-quantities.md) |
| [Similarity measures](similarity-measures.md) | Jaccard, containment, Tversky, the WL kernel, MinHash | [guide 02](../guides/02-material-similarity.md), [api/01](../api/01-similarity-scalars.md) |
| [Predecessors and cold start](predecessors-and-cold-start.md) | Anti-correlated consumption, analogs with history | [guide 03](../guides/03-predecessors-and-cold-start.md), [api/02](../api/02-material-search.md) |
| [ERP BOM models](erp-bom-models.md) | SAP and Dynamics 365 BOM tables, alternatives, validity, clients and companies | [guide 12](../guides/12-from-erp-extract.md), [api/05](../api/05-erp-transformations.md) |
| [Embeddings and fusion](embeddings-and-fusion.md) | Transactional, Jaccard and textual embeddings, fusion, vss indexes | [api/04](../api/04-embeddings.md) |
| [Classification and calibration](classification-and-calibration.md) | Pivot classification, the name floor, review queues | [guide 06](../guides/06-classification-and-review.md), [api/14](../api/14-classification-and-review.md) |
| [Evaluation and scale](evaluation-and-scale.md) | Ranking and calibration metrics, cluster bootstrap, LSH banding | [guide 07](../guides/07-evaluation-and-tuning.md), [guide 08](../guides/08-scale-and-dedup.md), [api/15](../api/15-evaluation.md), [api/16](../api/16-lsh-and-clustering.md) |
| [AAS mapping](aas-mapping.md) | How submodels become trees, semantic keys, levelType, constraints | [guide 11](../guides/11-aas-first-match.md), [api/17](../api/17-aas.md) |

Design records (how the design was decided, not the current contract) are in
[requirements-matching](../requirements-matching/README.md) and [background](../background/README.md).

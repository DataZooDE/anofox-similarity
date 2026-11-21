# Requirements matching: design records

> The requirements, design and review history of coverage matching (tree_coverage and its
> relatives). They record decisions as they were taken; later amendments are dated inside.
> For current behaviour see [guide 04](../guides/04-coverage-matching.md) and
> [the coverage kernel](../api/13-coverage-kernel.md).

| File | Content | Reader |
|---|---|---|
| [tree.schema.json](tree.schema.json) | JSON schema of a property tree, the input of `tree_from_json` | **users**: the contract for extraction output |
| [BRD.md](BRD.md) | Business requirements (BR-xx) | design record |
| [HLD.md](HLD.md) | High-level design: matching semantics, kernel, stages | design record |
| [HANDOVER.md](HANDOVER.md) | Working brief for the implementation (R0–R5) | design record |
| [REVIEW_LOG.md](REVIEW_LOG.md) | Crew reviews and their resolutions | design record |

`tree.schema.json` stays at this path: `tree_from_json`'s description and the API pages link it.

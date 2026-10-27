# Round 4: C-parts harmonisation as a buyer

I generated the 91-line fictional catalogue in `examples/c_parts_harmonisation/` and ran its SQL with the extension-linked DuckDB 1.5.6 shell. The exact-key workflow recovered 40/40 reviewed duplicate pairs, reported 51 clusters and a maximum observed supplier price spread of 59.1%. The fixture is small enough to inspect by hand.

## Surprise: a plausible pair chain becomes one conflicting cluster

**Severity: medium.** This is the documented transitive behavior of `cluster_matches`, but its output can be misread as an interchangeable group. A buyer needs a second pass over every member, especially when missing attributes are treated as wildcards.

Minimal reproduction, in the extension-linked shell:

```sql
CREATE TABLE links(a VARCHAR, b VARCHAR, score DOUBLE);
INSERT INTO links VALUES ('M8x25', 'M8-unknown', 0.95),
                         ('M8-unknown', 'M8x30', 0.95);
SELECT entity_id, cluster_id, cluster_size
FROM cluster_matches('links', threshold := 0.9) ORDER BY entity_id;
```

**Actual:** all three rows have `cluster_id = M8-unknown` and `cluster_size = 3` (the smallest text ID is `M8-unknown`). **Expected for an approved substitution set:** M8x25 and M8x30 must not be accepted in one group. The function cannot know the lengths from pair IDs, so the practical fix is the SQL whole-cluster guard in the example. A product fix could expose an optional post-cluster validation hook or return edge provenance to make a review UI easier to build.

## Verdict

The folder demonstrates the buyer's first pass convincingly: deterministic mixed-language data, reviewed synonyms, exact matching, a visible conflict, and an actionable price spread. A customer would still ask who approves equivalence of standards, grades and coatings, how unknown dimensions are resolved, and how a proposed survivor part is chosen. Those approvals are outside the clustering function and should remain explicit in the purchasing workflow.

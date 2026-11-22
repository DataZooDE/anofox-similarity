# Coverage Semantics

> What `tree_coverage` measures, which rules produce the number, and why the defaults are set as
> they are. For the calls, see [guide 04](../guides/04-coverage-matching.md) and
> [api/13](../api/13-coverage-kernel.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Coverage Is Directional

Similarity is symmetric: how alike are A and B? Coverage asks one direction only: does the offer B
provide everything the request A asks for? Extra items in B do not lower coverage. Missing items,
too few instances and values that miss their limit do.

| | Similarity (Jaccard) | Coverage |
|---|---|---|
| Question | How alike are A and B? | Does B provide what A asks for? |
| Symmetric | yes | no: the request is always the first argument |
| An offer with more items | scores lower | scores the same |
| Result | one number | a number per request, plus a gap per requirement |

A request is a tree of **items** (things asked for, each with a class, a count and attributes).
Containers, sections and OR groups only structure it; they are not compared. An offer is a tree
of the same kind. Values are in base units with a dimension, so `max. 180 s` and `3 min` compare
directly (see [guide 05](../guides/05-quantities-and-specs.md)).

## Two Stages

1. **Recall (stage 1).** Every candidate tree is reduced to the multiset of its item classes,
   with counts. Candidates are ranked by how much of the request's multiset they contain
   (`containment`, plus a small Jaccard tie-break) when every tree is reviewed, else by weighted
   Jaccard, which is more robust to wrong labels. The top `k` (default 20) go on.
2. **Coverage (stage 2).** The kernel compares the request with each recalled candidate item by
   item, with values, counts and the tree structure, and returns the coverage and the trace.

Stage 1 is cheap and only a filter: a candidate it does not recall is never scored. Raise `k`, or
pass `candidates`, when you quote more than the top result.

## The Kernel

For a request item *q* and a candidate item *c*:

```
attr(x, y)  = gate · (mu·name_sim + 1 − mu) · value_satisfies(x, y).score     (categories: compat_score;
              mu counts only for name_rule 'vector', else 0)
a(q, c)     = mean over q's attributes x of the best attr(x, ·) over c's       (1 without attributes)
L(q, c)     = label agreement: Bhattacharyya of the class probabilities, or 0/1 with hard labels
score(q, c) = label_weight·L + (1 − label_weight)·L·a                          (default label_weight 0)
req(q)      = best score × min(1, Σ counts of compatible items / count(q))
coverage    = mean req(q) over the request's items
```

- **gate** is 0 when the dimensions differ (amperes against volts), a unit is unknown, the
  currencies differ, or two dictionary keys differ. A gated pair is a mismatch, not a weak match.
- **value_satisfies** scores the value by its bound: at least, at most, within (the offer covers
  the range), equals (inside the tolerance or the stated span), or a point. Tolerance and soft band
  give partial scores near the limit.
- **Counts** are extensive. They multiply down the tree, add across AND siblings, and never add
  across OR alternatives. Four massage zones asked, two offered: the count factor is 0.5.

A worked example. The request asks for seat heating (two limits) and four massage zones. OFF-1
has the heating with a current over the limit and two zones; OFF-2 has eight zones and no heating:

```sql
CREATE OR REPLACE TABLE docs AS SELECT * FROM (VALUES
  ('{"tree_id": "REQ", "root": {"node_id": "REQ", "kind": "container", "children": [
     {"node_id": "REQ.1", "kind": "item", "label": "seat heating", "class_id": "heating", "attributes": [
        {"attr_id": "t", "name": "time to 37 °C", "text": "max. 180 s"},
        {"attr_id": "i", "name": "current draw", "text": "max. 8 A"}]},
     {"node_id": "REQ.2", "kind": "item", "label": "massage", "class_id": "massage", "count": 4}]}}'),
  ('{"tree_id": "OFF-1", "root": {"node_id": "O1", "kind": "container", "children": [
     {"node_id": "O1.1", "kind": "item", "label": "heater mat", "class_id": "heating", "attributes": [
        {"attr_id": "t", "name": "heat-up time", "text": "150 s"},
        {"attr_id": "i", "name": "current", "text": "9 A"}]},
     {"node_id": "O1.2", "kind": "item", "label": "massage", "class_id": "massage", "count": 2}]}}'),
  ('{"tree_id": "OFF-2", "root": {"node_id": "O2", "kind": "container", "children": [
     {"node_id": "O2.1", "kind": "item", "label": "massage", "class_id": "massage", "count": 4},
     {"node_id": "O2.2", "kind": "item", "label": "massage", "class_id": "massage", "count": 4}]}}')
) t(doc);
CREATE OR REPLACE TABLE nodes AS SELECT unnest(tree_from_json(doc)) AS n FROM docs;
CREATE OR REPLACE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM nodes;
CREATE OR REPLACE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM nodes;

SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('REQ', candidates := ['OFF-1', 'OFF-2']) ORDER BY candidate_id;
-- => OFF-1 | 0.5 | 2
-- => OFF-2 | 0.5 | 1
```

Both offers cover half the request, for different reasons. OFF-1: the heating meets one of two
limits (a = 0.5) and two of four zones give a count factor of 0.5, so each requirement scores 0.5
and both are gaps. OFF-2: no heating (0), the massage in full (1). The same coverage can mean
"almost everything, a little short" or "half missing": read `n_gaps` and the trace, not only the
number.

## Assignment: Who May Serve Which Requirement

With `assignment := 'partition'` each candidate item serves the one requirement it agrees with
most. An item cannot stand in for another function, and one item cannot serve two requirements:
a request for a 10 W and a 90 W motor is covered by an offer of a 95 W and a 20 W motor whatever
the item ids, but one 95 W motor cannot serve both. With `'best_match'` every requirement takes its
best item independently. That is robust to wrong labels, but a different function can then stand
in for a missing one. `'auto'` (default) uses partition when both trees are reviewed (every class
from `class_id` or the reviewed vocabulary) and best match otherwise.

## Alternatives: One Configuration

OR groups in a candidate are alternatives of one product: module variants, configurations. With
`alternatives := 'joint'` (default) the whole request is held to one alternative per group: a
station that needs a quiet conveyor and a high stack scores like the best single variant, not
like a mix of two variants nobody can build (in [tender_reuse](../../examples/tender_reuse/) MOD-FEED
scores 0.9333 jointly and 1.0 per requirement). `'per_requirement'` lets each requirement pick its
own variant; use it to see what each variant could cover.

## Attribute Names and Keys

A requirement's value is compared with the offered values of the same dimension on the matched
item. When several qualify (two lengths: travel and width), something must say which one answers:

1. **Dictionary keys** (`attr_key`, for AAS the semanticId) decide when both sides carry one.
   Equal keys match; different keys are a hard gate. Once the item has the requirement's key,
   unkeyed values do not stand in.
2. **Names** decide otherwise. With `name_vectors`, the calibrated cosine of the name vectors
   (`name_rule` `vector`), and `mu` (default 0.2) lowers the score by how poorly the names agree.
   Without vectors, word overlap (`tokens`) picks the value but does not lower its score: word
   overlap across languages is often 0 and says little.

So names choose and, with vectors, discount; they never create a match on their own. Without keys
an offered width can answer a travel clause when its value fits (see
[guide 13](../guides/13-tender-to-trees.md)); give `attr_key` where a dimension repeats on an item.

## Class Hierarchy

By default classes are flat: a requirement for hot-plate sealing gets nothing from an offer of heat
sealing. With `class_hierarchy` (`class_id, parent_class_id`) the relation is *is-a*:

```
rel(required k, provided l) = 1     same class, or l a subclass of k (specialized)
                            = c^d   l a superclass of k at distance d (generalized)
                            = 0     otherwise
```

A hot-plate sealer is a heat sealer, so it fully meets a heat-sealing requirement. A general heat
sealer offered for a hot-plate requirement gets `generalization_credit` c per level (default 0).
Generalized items still have to pass the partition and count gates, so superclass items cannot hide
a count shortfall. Stage 1 does not use the hierarchy.

## Gaps and the Trace

`is_gap` marks a requirement (an item) that has no counterpart, too few instances, or an attribute
below its limit. `n_gaps` counts these items, not attributes. The trace has one row per required
attribute with both values (`req_value`, `prov_value`), the rule, the value score and the
distance: relative for limits, a share for ranges and points, kelvin for temperature limits. The
trace and the ranking come from the same pass, so the trace explains exactly the score you see.

## Why These Defaults

| Default | Why |
|---|---|
| `label_weight` 0 | Coverage is about values: a matching class with failing values must not look covered |
| `mu` 0.2, only with name vectors | Names are weak evidence: they choose between values, a small discount with vectors |
| `assignment` `'auto'` | Partition prevents substitution, but needs trustworthy (reviewed) classes |
| `alternatives` `'joint'` | An offer is one buildable configuration |
| `k` 20 | Recall is only a filter and cheap; stage 2 costs per candidate. Raise it, or pass `candidates`, when you quote more than the top result |
| `generalization_credit` 0 | A more general offer is not evidence that the specific need is met |

The design history (requirements, rejected options, amendments) is in the
[HLD](../requirements-matching/HLD.md); it records how these rules were decided, not the current
contract.

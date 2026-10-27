# Property trees

> Load, validate and summarise request and offer trees.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Property trees

Requests and offers are trees: `tree_nodes(tree_id, node_id, parent_id, node_kind,
edge_type, label, class_id, count[, source, context])` and `tree_attributes(tree_id, node_id,
attr_id, name, dimension, unit, lo, hi[, bound, category])`, with values in base units. Item
nodes are compared. Containers and variant groups are transparent, so grouping levels do not
have to align. OR edges mark alternatives, and counts are extensive: they multiply
down the tree, add across AND siblings and never add across OR alternatives.

| Function | Purpose |
|---|---|
| `tree_from_json(json [, tree_id [, lang [, unit_table]]])` | Scalar. Turns an extracted tree (JSON, [schema](../requirements-matching/tree.schema.json)) into a LIST of node STRUCTs with normalised, keyed attributes. Works over many documents: `SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM read_text('specs/*.json'))`. Structured value fields win over the text; text-only values are parsed with `parse_quantity`. Errors name the node: a non-positive `count`, or a key close to a known one (`childs`, `txt`), which would drop data silently. Other keys are ignored, so the JSON can carry metadata |
| `tree_anchors(nodes_table [, attrs_table], overrides_table, label_table, tree_ids)` | One row per item: class, reviewed, `effective_count`, `depth`, `anchor_path` (ancestors), `variant_path` (OR choices, the item included), `n_attributes` |
| `tree_class_multiset(nodes_table, overrides_table, label_table, tree_ids)` | `(entity_id, feature_id, weight, reviewed)` for stage 1; the elementwise maximum across OR alternatives (an optimistic bound) |
| `tree_multiset(nodes LIST)` | The scalar behind it, for custom pipelines |
| `validate_trees(nodes_table [, attrs_table], tree_ids := ...)` | `(tree_id, node_id, attr_id, issue, severity, detail)`. Errors: duplicate_node, bad_node_kind, bad_edge_type, bad_count, orphan, no_root, multiple_roots, cycle, unreachable, too_deep, attribute_on_container, orphan_attribute, bad_interval, bad_bound. Warnings: mixed_edge_types, unknown_unit, mixed_dimensions (per source and name) |

Examples:

```sql
SELECT * FROM validate_trees('programme_nodes', 'programme_attributes') WHERE severity = 'error';
SELECT node_id, class_id, effective_count, anchor_path FROM tree_anchors('programme_nodes', tree_ids := ['P001']);
CREATE TABLE programme_index AS SELECT * FROM tree_class_multiset('programme_nodes', overrides_table := 'vocabulary_overrides');
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content, NULL, 'de')) AS n FROM read_text('specs/*.json'));
```

**Class resolution** (shared by both stages): `class_id` → `overrides_table` (the reviewed
vocabulary: `wording` matched exactly against `label`; a NULL `source` matches any source) →
`label_table` (classifier output `tree_id, node_id, class_id, p`: arg-max, ties to the lowest
class). A tree is *reviewed* when every item got its class from `class_id` or the vocabulary.
Trees deeper than 256 levels are rejected (`tree_anchors` stops there); specification trees are
typically 4–20 levels deep.

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `tree_from_json`

*Scalar function*

Turns a tree from upstream extraction (JSON: {tree_id, source, lang, root: {node_id, kind, edge, label, context, class_id, count, attributes: [{attr_id, name, text, lo, hi, unit, bound, category}], children: [...]}}) into a LIST of node STRUCTs with normalised attributes (base units, dimension; text-only values are parsed with parse_quantity in the document's lang, or lang if given; unit_table extends the units seed as in parse_quantity). Attribute STRUCTs carry tree_id and node_id. Unnest it into tree_nodes and tree_attributes. A non-positive count, or a key close to a known one (childs, txt), is an error naming the node; other keys are ignored. Schema: docs/requirements-matching/tree.schema.json.

```sql
tree_from_json(json VARCHAR) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR)[])[]
tree_from_json(json VARCHAR, tree_id VARCHAR) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR)[])[]
tree_from_json(json VARCHAR, tree_id VARCHAR, lang ANY) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR)[])[]
tree_from_json(json VARCHAR, tree_id VARCHAR, lang ANY, unit_table ANY) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR)[])[]
```

**Examples**

```sql
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM read_text('requests/*.json'));
```

### `validate_trees`

*Table function*

Checks property trees and returns one row per issue (tree_id, node_id, attr_id, issue, severity, detail). Errors: duplicate_node, bad_node_kind, bad_edge_type, bad_count, orphan, no_root, multiple_roots, cycle, and with an attribute table attribute_on_container, orphan_attribute, bad_interval, bad_bound. Warnings: mixed_edge_types, unknown_unit, mixed_dimensions (one attribute name with several dimensions within a source).

```sql
validate_trees(nodes_table, attrs_table, overrides_table, label_table, tree_ids)
```

**Examples**

```sql
SELECT * FROM validate_trees('tree_nodes', 'tree_attributes');
```

### `tree_anchors`

*Table function*

Flattens property trees: one row per item node (containers are transparent) with its resolved class, depth, anchor_path (ancestor ids from the root), variant_path (the nodes with an OR edge on the way, i.e. the alternatives chosen), effective_count (product of the counts along the path) and n_attributes. Works at any depth.

```sql
tree_anchors(nodes_table, attrs_table, overrides_table, label_table, tree_ids)
```

**Examples**

```sql
SELECT * FROM tree_anchors('tree_nodes', 'tree_attributes');
SELECT * FROM tree_anchors('tree_nodes', tree_ids := ['M1']);
```

### `tree_class_multiset`

*Table function*

The class multiset per tree for stage-1 recall: entity_id (tree), feature_id (class), weight (summed counts, maximum across OR alternatives), reviewed (every item's class came from class_id or the reviewed vocabulary). Classes resolve in the order class_id, overrides_table (wording, source, class_id), label_table (classifier arg-max).

```sql
tree_class_multiset(nodes_table, attrs_table, overrides_table, label_table, tree_ids)
```

**Examples**

```sql
SELECT * FROM tree_class_multiset('tree_nodes');
SELECT * FROM tree_class_multiset('tree_nodes', overrides_table := 'vocabulary_overrides');
```

### `tree_multiset`

*Scalar function*

The class multiset of one tree given as a LIST of STRUCT(node_id, parent_id, node_kind, edge_type, class_id, count): counts multiply down the tree, add across AND siblings and take the elementwise maximum across OR alternatives. The building block of tree_class_multiset.

```sql
tree_multiset(nodes ANY) → MAP(VARCHAR, DOUBLE)
```

**Examples**

```sql
SELECT tree_multiset(list(struct_pack(node_id, parent_id, node_kind, edge_type, class_id, count))) FROM tree_nodes GROUP BY tree_id;
```

<!-- generated:functions:end -->

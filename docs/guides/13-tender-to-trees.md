# From a Tender to Trees

> Turn an extracted tender into request trees that `tree_coverage` can score, and keep the
> clause numbers for the bid.

**Use this guide to:**
- Know what upstream extraction must deliver (the extension reads no PDF)
- Load a tender JSON with `tree_from_json` and check it with `validate_trees`
- Key properties that share a dimension (`attr_key`), so one cannot answer for another
- Give the items a class before stage 2, and rank an archive against the tender
- Map gaps back to the clauses (page, must/should) with a sidecar table

Every block runs from the repository root, in order. The data is inline; the full worked case
is [examples/tender_reuse](../../examples/tender_reuse/README.md).

## What Upstream Delivers

The extension does not read PDFs, Word files or scanned pages. An extraction step (an LLM, a
form parser, a person) turns the tender into one JSON document per request:

- a tree of nodes: containers for chapters and stations, **items** for what must be supplied,
  with `count`;
- per item, the attributes as written: `name` and `text` (`mind. 240 mm`), or `lo`/`hi`/`unit`;
- optionally `attr_key` (a property id: ECLASS IRDI or your own) and `class_id`.

The format is [tree.schema.json](../requirements-matching/tree.schema.json). Values stay as text:
`tree_from_json` parses them with `parse_quantity` in the document's `lang`. `class_id` is
usually missing in a fresh extraction, so this guide leaves it out.

## Load the Tender

Two items: an electric seat track with two lengths, and a seat heater.

```sql
CREATE OR REPLACE TABLE tender_json AS SELECT '{
 "tree_id": "RFQ-7", "source": "OEM tender", "lang": "de",
 "root": {"node_id": "RFQ-7", "label": "Lastenheft Sitzschiene", "children": [
  {"node_id": "RFQ-7.1", "label": "Längsverstellung elektrisch", "count": 2,
   "attributes": [
    {"attr_id": "travel", "name": "Verstellweg", "text": "mind. 240 mm", "attr_key": "travel"},
    {"attr_id": "width",  "name": "Einbaubreite", "text": "max. 450 mm", "attr_key": "width"}]},
  {"node_id": "RFQ-7.2", "label": "Sitzheizung", "count": 2,
   "attributes": [{"attr_id": "power", "name": "Heizleistung", "text": "mind. 60 W"}]}]}}' AS content;
CREATE OR REPLACE TABLE request_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM tender_json);
CREATE OR REPLACE TABLE request_attributes AS SELECT unnest(attributes, recursive := true) FROM request_nodes;
SELECT node_id, attr_id, attr_key, dimension, lo, hi, bound FROM request_attributes ORDER BY node_id, attr_id;
-- => RFQ-7.1 | travel | travel | length | 0.24 | 0.24 | at_least
-- => RFQ-7.1 | width | width | length | 0.45 | 0.45 | at_most
-- => RFQ-7.2 | power | NULL | power | 60.0 | 60.0 | at_least
```

Values are in base units (m, W). Read every row once: a clause read wrongly scores as a gap of
the offer, and no function raises an error for it ([Quantities and specs](05-quantities-and-specs.md)
lists the parser's traps). Then check the structure:

```sql
SELECT count(*) AS issues FROM validate_trees('request_nodes', 'request_attributes');
-- => 0
```

`validate_trees` reports duplicates, orphans, bad counts, bad intervals and unknown units
([property trees](../api/12-property-trees.md)).

## Key Properties That Share a Dimension

`Verstellweg` and `Einbaubreite` are both lengths. Without `attr_key`, a required value is
compared with every offered value of the same dimension on the matched item, so the offer's
width can answer the travel. Give such properties a key on both sides; equal keys decide the
match and different keys are a gate. Two archive lines, both with classes (the archive is
reviewed):

```sql
CREATE OR REPLACE TABLE archive_json AS SELECT * FROM (VALUES
('{"tree_id": "OFF-A", "lang": "en", "role": "provided", "root": {"node_id": "OFF-A", "label": "Seat track A", "children": [
  {"node_id": "OFF-A.1", "label": "Power seat track", "class_id": "seat_track", "count": 2, "attributes": [
    {"attr_id": "travel", "name": "travel", "text": "250 mm", "attr_key": "travel"},
    {"attr_id": "width", "name": "width", "text": "420 mm", "attr_key": "width"}]},
  {"node_id": "OFF-A.2", "label": "Seat heater", "class_id": "seat_heater", "count": 2, "attributes": [
    {"attr_id": "power", "name": "heating power", "text": "65 W"}]}]}}'),
('{"tree_id": "OFF-B", "lang": "en", "role": "provided", "root": {"node_id": "OFF-B", "label": "Seat track B", "children": [
  {"node_id": "OFF-B.1", "label": "Power seat track", "class_id": "seat_track", "count": 2, "attributes": [
    {"attr_id": "travel", "name": "travel", "text": "200 mm", "attr_key": "travel"},
    {"attr_id": "width", "name": "width", "text": "420 mm", "attr_key": "width"}]},
  {"node_id": "OFF-B.2", "label": "Seat heater", "class_id": "seat_heater", "count": 2, "attributes": [
    {"attr_id": "power", "name": "heating power", "text": "65 W"}]}]}}')) t(content);
CREATE OR REPLACE TABLE archive_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM archive_json);
CREATE OR REPLACE TABLE archive_attributes AS SELECT unnest(attributes, recursive := true) FROM archive_nodes;
```

OFF-B offers only 200 mm of travel. The effect of the keys is shown after the classes below.

## Classes Before Stage 2

Both stages match items by class. The tender has none yet, so scoring stops:

```sql
SELECT * FROM tree_coverage('RFQ-7', candidates := ['OFF-A', 'OFF-B'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes',
  candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'archive_attributes');
```

```text
Invalid Input Error: tree_coverage: query tree 'RFQ-7' has no classified items (give class_id, overrides_table or label_table)
```

For wordings you know, a reviewed vocabulary (`wording` matched exactly against `label`) is
enough. For new wordings, classify them and review the uncertain ones first
([Classification and review](06-classification-and-review.md)); pass the result as `label_table`.

```sql
CREATE OR REPLACE TABLE tender_vocabulary AS SELECT * FROM (VALUES
  ('Längsverstellung elektrisch', NULL, 'seat_track'),
  ('Sitzheizung', NULL, 'seat_heater')) t(wording, source, class_id);
SELECT candidate_id, coverage, n_gaps, rank FROM tree_coverage('RFQ-7', candidates := ['OFF-A', 'OFF-B'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes',
  candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'archive_attributes',
  overrides_table := 'tender_vocabulary') ORDER BY rank;
-- => OFF-A | 1.0 | 0 | 1
-- => OFF-B | 0.75 | 1 | 2
```

OFF-B misses the travel: the seat track item scores 0.5 (one of two values), the heater 1.

Without the keys, OFF-B looks like a full cover, because its 420 mm width answers
"mind. 240 mm":

```sql
CREATE OR REPLACE TABLE request_attributes_nokey AS SELECT * REPLACE (NULL::VARCHAR AS attr_key) FROM request_attributes;
CREATE OR REPLACE TABLE archive_attributes_nokey AS SELECT * REPLACE (NULL::VARCHAR AS attr_key) FROM archive_attributes;
SELECT attr_id, req_value, prov_value, value_score FROM tree_coverage_trace('RFQ-7', candidates := ['OFF-B'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes_nokey',
  candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'archive_attributes_nokey',
  overrides_table := 'tender_vocabulary') ORDER BY req_node_id, attr_id;
-- => travel | at_least 0.24 m | 0.42 m | 1.0
-- => width | at_most 0.45 m | 0.2 m | 1.0
-- => power | at_least 60 W | 65 W | 1.0
```

If the extraction cannot deliver keys, give each such property its own item and class instead
(as [examples/second_source](../../examples/second_source/README.md) does).

## Gaps by Clause

The tree carries no clause numbers, pages or must/should. Keep them in a sidecar table keyed by
node and attribute, filled by the same extraction step, and join it to the trace:

```sql
CREATE OR REPLACE TABLE clauses AS SELECT * FROM (VALUES
  ('C-03', 12, 'must',   'RFQ-7.1', 'travel', 'Verstellweg: mind. 240 mm'),
  ('C-04', 12, 'must',   'RFQ-7.1', 'width',  'Einbaubreite: max. 450 mm'),
  ('C-09', 15, 'should', 'RFQ-7.2', 'power',  'Heizleistung: mind. 60 W')) t(clause_id, page, must_should, node_id, attr_id, text);
SELECT c.clause_id, c.page, c.must_should, t.req_value, t.prov_value, t.value_score
FROM tree_coverage_trace('RFQ-7', candidates := ['OFF-B'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes',
  candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'archive_attributes',
  overrides_table := 'tender_vocabulary') t
JOIN clauses c ON c.node_id = t.req_node_id AND c.attr_id = t.attr_id
WHERE t.value_score < 1 ORDER BY c.clause_id;
-- => C-03 | 12 | must | at_least 0.24 m | 0.2 m | 0.0
```

The kernel does not weight must against should; decide the bid on the must clauses, and use
coverage to rank. [examples/tender_reuse](../../examples/tender_reuse/README.md) adds
`must_missed` and `must_coverage` per line.

## Next

- [Coverage matching](04-coverage-matching.md): the stages, counts, OR alternatives and the trace
- [Quantities and specs](05-quantities-and-specs.md): what the parser reads, bounds and tolerances
- [Classification and review](06-classification-and-review.md): classes for new wordings

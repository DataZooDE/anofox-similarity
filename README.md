<p align="center">
  <img src="docs/assets/logo.svg" alt="Anofox Logo" width="80" height="80">
</p>

<h1 align="center">Anofox Similarity</h1>
<p align="center"><strong>Product similarity and requirements coverage for DuckDB</strong></p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-BSL%201.1-blue.svg" alt="License: BSL 1.1"></a>
  <a href="https://duckdb.org"><img src="https://img.shields.io/badge/DuckDB-1.5.6%20%7C%202.0-green.svg" alt="DuckDB 1.5.6 and 2.0"></a>
  <a href="https://github.com/DataZooDE/anofox-similarity/actions/workflows/MainDistributionPipeline.yml"><img src="https://github.com/DataZooDE/anofox-similarity/actions/workflows/MainDistributionPipeline.yml/badge.svg" alt="Build"></a>
</p>

A DuckDB extension that answers two questions on enterprise master data, in pure SQL:

- **Which products are alike?** Find similar materials from their bills of materials, detect
  predecessors from consumption history, and pick analogs to forecast new products.
- **Which past solution covers this request?** Rank offers, designs or past programmes by how much
  of a structured request they cover, with the gaps listed and explained, including values with
  units and direction ("at most 38 dB(A)").

It works on the data ERP systems already have (SAP, Dynamics 365 and others): BOMs, goods
movements, material texts and extracted specifications. Bring your own vectors where meaning
matters; the extension does not ship language models.

## ✨ Key Features

### 🔩 Material similarity (20 functions)
- **Set and graph similarity**: Jaccard and Weisfeiler-Lehman kernel similarity of BOMs, top-k
  search (`find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`)
- **Predecessors and cold start**: detect a predecessor from anti-correlated consumption, find
  analogs with history for a new product (`infer_predecessors`, `cold_start_analogs`)
- **BOM traversal**: explosion, where-used, common components, property graph helpers

### 📐 Coverage matching (16 functions)
- **Directional set measures**: containment, Tversky and weighted measures that do not punish a
  richer offer (`containment`, `weighted_containment`, `find_covering_entities`)
- **Quantities**: parse EN/DE values with units and bounds, compare them with direction and
  tolerance (`parse_quantity`, `value_satisfies`, `compat_score`)
- **Property trees and the coverage kernel**: load extracted specs from JSON, rank candidates by
  coverage, and list every gap with the failing value and its distance (`tree_from_json`,
  `tree_coverage`, `tree_coverage_trace`)

### 🏷️ Classification and review (8 functions)
- Map items onto a catalogue with your own vectors (`classify_to_catalogue`, `maxsim`)
- Review the few wordings that matter most, and send uncertain items to an external reviewer as
  JSON (`vocabulary_review_queue`, `apply_vocabulary_review`, `uncertain_decisions`)

### 📊 Evaluation and scale (13 functions)
- Recall@k, MRR, regret, calibration, the auto-accept threshold for a target precision,
  cluster bootstrap intervals and a weight fit (`eval_ranking`, `threshold_at_precision`, …)
- Weighted MinHash and size-partitioned LSH for large archives; duplicate clustering
  (`find_covering_entities_lsh`, `cluster_matches`)

### 🏭 ERP and embeddings (22 functions)
- SAP and Dynamics 365 transformations, transactional and Jaccard embeddings, HNSW indexes
- Textual embeddings are a **placeholder** in the released build; see
  [embeddings](docs/api/04-embeddings.md)

## 📋 Table of Contents

- [Installation](#installation)
- [Quick Start: similar materials](#-quick-start-similar-materials)
- [Quick Start: coverage matching](#-quick-start-coverage-matching)
- [Documentation](#-documentation)
- [Multi-Language Support](#-multi-language-support)
- [Development](#-development)
- [License](#-license)

## Installation

The extension is not yet published in the DuckDB community repository or in DataZoo's
repository (`INSTALL anofox_similarity FROM community` returns 404). Build it from source:

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-similarity.git
cd anofox-similarity
GEN=ninja make release
```

Then use the built shell, which has the extension linked in:

```bash
./build/release/duckdb
```

or load the binary into another DuckDB 1.5.6 shell started with `-unsigned`:

```sql
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
SELECT jaccard_similarity(['A', 'B', 'C'], ['A', 'B', 'D']);  -- 0.5
```

## 🚀 Quick Start: similar materials

```sql
-- Bills of materials: which components each product uses
CREATE OR REPLACE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
  ('PUMP-A', 'SEAL-001'), ('PUMP-A', 'BEARING-X'), ('PUMP-A', 'MOTOR-M1'),
  ('PUMP-B', 'SEAL-001'), ('PUMP-B', 'BEARING-X'), ('PUMP-B', 'MOTOR-M1'), ('PUMP-B', 'GASKET-Y'),
  ('PUMP-C', 'FILTER-F1'), ('PUMP-C', 'VALVE-V2');

-- The products most similar to PUMP-A by shared components
SELECT material_id, similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-A', 3);
-- PUMP-B | 0.75 | 3 | 4
-- PUMP-C | 0.0  | 0 | 5
```

A new product has no sales history. Find its predecessor from consumption that declines while
the new product ramps up, and analogs with enough history to forecast from:

```sql
CREATE OR REPLACE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
  ('OLD-PUMP', 'SEAL-001'), ('OLD-PUMP', 'BEARING-X'), ('OLD-PUMP', 'MOTOR-M1'),
  ('NEW-PUMP', 'SEAL-001'), ('NEW-PUMP', 'BEARING-X'), ('NEW-PUMP', 'MOTOR-M1');

-- Weekly consumption: OLD-PUMP declines while NEW-PUMP ramps up
CREATE OR REPLACE TABLE goods_movements AS
SELECT 'OLD-PUMP' AS material_id, DATE '2023-01-02' + 7 * w::INTEGER AS movement_date, 200.0 - 5 * w AS quantity
FROM range(0, 40) t(w)
UNION ALL
SELECT 'NEW-PUMP', DATE '2023-01-02' + 7 * w::INTEGER, 5.0 * w - 50
FROM range(10, 52) t(w);

SELECT predecessor_id, round(confidence, 2) AS confidence, correlation, overlapping_weeks
FROM infer_predecessors('NEW-PUMP');
-- OLD-PUMP | 0.79 | -1.0 | 38

SELECT material_id, similarity, history_months, first_usage, last_usage
FROM cold_start_analogs('NEW-PUMP', 5, min_history_months := 6);
-- OLD-PUMP | 1.0 | 9 | 2023-01-02 | 2023-10-02
```

Predecessor detection needs dense weekly history; sparse movements return no rows. See the
[guide](docs/guides/03-predecessors-and-cold-start.md).

## 📐 Quick Start: coverage matching

A request asks for a set of items with values; past programmes provide items with values. Which
programme covers the request, and what is missing? This runs on the small fixture in the
repository:

```sql
CREATE OR REPLACE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');

-- Rank the candidates for request Q1 by the share of it they cover
SELECT candidate_id, round(coverage, 3) AS coverage, n_requirements, n_gaps
FROM tree_coverage('Q1', k := 3, mu := 0.0);
-- S1 | 1.0 | 4 | 0
-- S6 | 1.0 | 4 | 0
-- S2 | 0.5 | 4 | 2

-- Why S2 falls short: the gap list, value by value
SELECT req_node_id, attr_name, req_value, prov_value, value_score
FROM tree_coverage_trace('Q1', candidates := ['S2'], mu := 0.0)
WHERE is_gap
ORDER BY req_node_id, attr_name;
-- Q1.1 | current draw  | at_most 8 A     | 9 A    | 0.0
-- Q1.1 | time to 37 °C | at_most 180 s   | 240 s  | 0.0
-- Q1.3 | height travel | at_least 0.06 m | 0.05 m | 0.0
```

`mu := 0.0` switches off the attribute-name discount, which needs name vectors to be meaningful.
The [coverage-matching guide](docs/guides/04-coverage-matching.md) covers classes, values,
alternatives and large archives; the [examples](examples/coverage_matching/) run the full
seating benchmark.

## 📚 Documentation

| | |
|---|---|
| **[Guides](docs/guides/README.md)** | Task-based guides: getting started, material similarity, predecessors and cold start, coverage matching, quantities, classification and review, evaluation, scale and dedup |
| **[API reference](docs/API_REFERENCE.md)** | Every function by topic, with signatures, descriptions and examples |
| **[Examples](examples/README.md)** | Runnable SQL files on data in this repository |
| **[Tutorials](docs/guides/tutorials/)** | AdventureWorks walkthrough; SAP walkthrough (bring your own extract) |
| **[Background](docs/background/)** | Design notes and research (not the product contract) |

On DuckDB 1.5.6, named arguments of **scalar** functions bind by position: give them in the
documented order. Table functions bind by name. Everything works on a read-only database.

## 🌍 Multi-Language Support

The extension is SQL; any DuckDB client runs it.

| Language | Client | Example |
|---|---|---|
| **SQL** | DuckDB CLI | `SELECT * FROM find_similar_materials_jaccard('PUMP-A', 3);` |
| **Python** | `duckdb` package | `duckdb.sql("SELECT * FROM tree_coverage('Q1', k := 3)")` |
| **R** | `duckdb` package | `dbGetQuery(con, "SELECT ...")` |
| **Java** | JDBC driver | `stmt.executeQuery("SELECT ...")` |
| **Node.js** | `@duckdb/node-api` | `connection.run("SELECT ...")` |

Load the extension binary in the client the same way as in the shell (`LOAD '<path>'`, with
unsigned extensions allowed).

## 📦 Development

### Prerequisites

A C++17 compiler, CMake, Ninja and Git. The build fetches DuckDB as a submodule.

### Build and test

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-similarity.git
cd anofox-similarity
GEN=ninja make release                       # build the extension and a DuckDB shell

DATAZOO_DISABLE_TELEMETRY=1 make test        # the whole SQL test suite
./build/release/test/unittest "test/sql/coverage/*"   # one area
./build/release/test/unittest -s "[jaccard]"          # one tag, with output
```

The SQL tests live in `test/sql/` ([overview](test/README.md)). Examples run with
`scripts/run_examples.sh`. After changing a function's description, parameters or examples,
regenerate the reference with `python3 scripts/generate_api_reference.py`.

### Telemetry

The extension sends anonymous usage events (function names, no data). Disable them with
`DATAZOO_DISABLE_TELEMETRY=1` or `SET anofox_telemetry_enabled = false;` ([details](TELEMETRY.md)).

## 📄 License

**Business Source License 1.1 (BSL 1.1)**

✅ **Free for production use**: use it internally in your business
✅ **Free for development and research**

❌ **No hosted service**: you may not offer it to third parties as a hosted or embedded service

🔄 **Converts to MPL 2.0**: each version becomes Mozilla Public License 2.0 four years after its
release (the license's Change Date is five years after first publication; the earlier of the
two applies)

See [LICENSE](LICENSE) for the full terms.

## 🤝 Contributing

Issues and pull requests are welcome. Please describe the data shape that shows a problem;
matching quality depends on real master data.

## 📞 Support

- **Documentation**: [docs/](docs/)
- **Issues**: [GitHub Issues](https://github.com/DataZooDE/anofox-similarity/issues)

## 🎓 Citation

```bibtex
@software{anofox_similarity,
  title = {Anofox Similarity: Product Similarity and Requirements Coverage for DuckDB},
  author = {Joachim Rosskopf and DataZoo GmbH},
  year = {2026},
  url = {https://github.com/DataZooDE/anofox-similarity}
}
```

## Feedback

If `anofox_similarity` misbehaves or a similarity score looks wrong, please
[open an issue](https://github.com/DataZooDE/anofox-similarity/issues). Matching quality
depends on real material masters and BOM structures we cannot reproduce here, so a
report with your data shape is the fastest path to a fix.

If it saved you time, a star on the repo helps other people find it.

The first time you load the extension in an interactive terminal each day, a small
banner says the same thing. It never prints when output is piped, in notebooks, or in
CI. Silence it with `SET datazoo_banner = false;` or `DATAZOO_NO_BANNER=1`.

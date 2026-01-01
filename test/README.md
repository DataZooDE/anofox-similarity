# anofox-similarity Test Harness

This directory contains the test infrastructure for the anofox-similarity DuckDB extension.

## Running Tests

### Run All Tests
```bash
make test
```

### Run Tests with Debug Build
```bash
make test_debug
```

### Run Specific Test File
```bash
./build/release/test/unittest "test/sql/functions/jaccard_similarity.test"
```

## Three-Tier Testing Architecture

| Tier | Dataset | Visibility | Purpose | CI/CD |
|------|---------|------------|---------|-------|
| **Tier 1** | Synthetic Generator | Public | Unit tests, edge cases, benchmarking | ✅ Every commit |
| **Tier 2** | Figshare Consumer Electronics | Public (CC0) | Algorithm validation, demos, accuracy testing | ✅ Nightly/Release |
| **Tier 3** | SAP Private Dataset | **Strictly Private** | Production validation, customer acceptance | ❌ Local only |

**Note:** Tier information is stored in test file metadata (`# tier: tier1|tier2|tier3`) for CI/CD filtering. Tests are organized by functionality, not by tier.

## Directory Structure

```
test/
├── README.md                           # This file
├── sql/
│   ├── functions/                      # Core SQL function tests
│   │   ├── jaccard_similarity.test
│   │   ├── find_similar_materials.test
│   │   ├── cold_start_analogs.test
│   │   └── infer_predecessors.test
│   │
│   ├── algorithms/                     # Algorithm implementation tests
│   │   ├── jaccard/                    # Jaccard component similarity
│   │   │   └── edge_cases.test
│   │   ├── wl_kernel/                  # Weisfeiler-Lehman graph kernel
│   │   │   └── wl_kernel.test
│   │   └── predecessor/                # Predecessor detection algorithm
│   │
│   ├── scenarios/                      # Test scenarios S1-S12
│   │   ├── s1_identical_boms.test
│   │   ├── s2_disjoint_boms.test
│   │   ├── s3_controlled_overlap.test
│   │   ├── s4_variant_clusters.test
│   │   ├── s5_hierarchical_bom.test
│   │   ├── s6_predecessor_chain.test
│   │   ├── s7_anticorrelation.test
│   │   ├── s8_phaseout.test
│   │   ├── s9_no_predecessor.test
│   │   ├── s10_wl_structure.test       # (tested in wl_kernel.test)
│   │   ├── s11_deep_hierarchy.test     # (tested in wl_kernel.test)
│   │   └── s12_transitive_chain.test
│   │
│   ├── data_structures/                # DATA-STRUCTURES.md alignment
│   │   ├── universal_schema/           # Universal BOM schema tests
│   │   ├── erp_transformations/       # ERP transformation tests
│   │   ├── embeddings/                 # Embedding storage tests
│   │   └── graph_queries/              # DuckPGQ graph tests
│   │
│   ├── indexing/                       # VSS/HNSW indexing tests
│   │   ├── vss_embedding_generation.test
│   │   ├── vss_index_creation.test
│   │   ├── vss_indexed_search.test
│   │   ├── vss_incremental_update.test
│   │   ├── vss_similarity_comparison.test
│   │   └── vss_sap_integration.test
│   │
│   ├── accuracy/                      # Accuracy metrics and validation
│   │   ├── synthetic_accuracy.test
│   │   ├── figshare_accuracy.test
│   │   ├── predecessor_f1.test
│   │   └── accuracy_functions.test
│   │
│   ├── datasets/                       # Dataset-specific validation tests
│   │   ├── synthetic/                  # Synthetic data validation
│   │   ├── figshare/                   # Figshare consumer electronics
│   │   │   ├── product_families.test
│   │   │   ├── cross_category.test
│   │   │   └── material_distribution.test
│   │   └── sap/                        # SAP private data (Tier 3)
│   │       ├── sap_bom_integration.test
│   │       └── sap_embedding_validation.test
│   │
│   ├── integrations/                   # Integration with other systems
│   │
│   └── extension/                      # Extension infrastructure tests
│       └── anofox_similarity.test
│
├── data/
│   ├── synthetic/                      # Generated synthetic test data
│   │   ├── materials.csv.gz
│   │   ├── bom_items.csv.gz
│   │   ├── goods_movements.csv.gz
│   │   ├── ground_truth.csv.gz
│   │   └── metadata.json
│   │
│   ├── figshare/                       # Figshare consumer electronics (Tier 2)
│   │   ├── materials.csv.gz
│   │   ├── bom_items.csv.gz
│   │   └── ground_truth.csv.gz
│   │
│   └── sap/                            # SAP private data (Tier 3)
│       ├── mara.parquet
│       ├── mast.parquet
│       ├── stko.parquet
│       ├── stpo.parquet
│       └── README.md
│
└── scripts/
    └── generate_synthetic_data.py      # Synthetic BOM generator
```

## Test Organization

Tests are organized by **functionality** (what is tested), not by tier or implementation phase:

- **`functions/`**: SQL function tests (jaccard_similarity, find_similar_materials, etc.)
- **`algorithms/`**: Algorithm implementation tests (jaccard, wl_kernel, predecessor)
- **`scenarios/`**: Test scenarios S1-S12 from CONCEPT.md Section 9.2
- **`data_structures/`**: Schema, transformations, embeddings, graph queries per DATA-STRUCTURES.md
- **`indexing/`**: VSS/HNSW indexing tests
- **`accuracy/`**: Accuracy metrics (not tied to a specific tier)
- **`datasets/`**: Dataset-specific validation (synthetic, figshare, sap)
- **`integrations/`**: Integration with other extensions/systems
- **`extension/`**: Extension infrastructure

## Tier Metadata

Each test file includes tier information in comments for CI/CD filtering:

```sql
# name: test/sql/functions/jaccard_similarity.test
# description: Tests for jaccard_similarity() scalar function
# tier: tier1  # Every commit CI/CD
# group: [functions]
```

```sql
# name: test/sql/datasets/figshare/product_families.test
# description: Figshare product family clustering validation
# tier: tier2  # Nightly/Release only
# group: [datasets, figshare]
```

```sql
# name: test/sql/datasets/sap/sap_production_validation.test
# description: Production validation with private SAP data
# tier: tier3  # Local only, never in CI/CD
# group: [datasets, sap]
```

## Regenerating Synthetic Data

The synthetic test data is generated with a fixed seed for reproducibility:

```bash
python3 test/scripts/generate_synthetic_data.py --output test/data/synthetic/ --seed 42
```

Options:
- `--output`: Output directory (default: `test/data/synthetic`)
- `--seed`: Random seed for reproducibility (default: 42)

## Test Scenarios

### Synthetic Test Scenarios (S1-S12)

| Scenario | Description | Expected Jaccard |
|----------|-------------|------------------|
| **S1** | Identical BOMs | 1.0 |
| **S2** | Disjoint BOMs | 0.0 |
| **S3** | Controlled Overlap (3 shared, 4 unique) | 3/7 = 0.4286 |
| **S4** | Variant Clusters (90%, 70%, 50% retention) | 0.8182, 0.5385, 0.3333 |
| **S5** | Hierarchical BOM (multi-level) | 1/3 = 0.3333 |
| **S6** | Predecessor Chain (70% retention) | 0.5385 |
| **S7** | Clear predecessor with anti-correlation | High confidence, negative correlation |
| **S8** | Gradual phase-out with extended overlap | Long overlap period (≥30 weeks) |
| **S9** | No predecessor (genuinely new product) | 0 predecessors found |
| **S10** | Same components, different structure | WL captures structural difference |
| **S11** | Deep hierarchy (4-level BOM) | WL captures depth |
| **S12** | Transitive predecessor chain (6 generations) | Direct predecessors detected |

### Edge Cases

Edge cases are tested in `algorithms/jaccard/edge_cases.test`:
- **Empty sets**: Products without BOMs, single-component BOMs
- **NULL handling**: NULL values in descriptions, groups, component IDs
- **Type coercion**: VARCHAR vs INTEGER vs UUID component IDs

### Figshare Tests

Expected product family clusters:
- **iPhone family** (4, 5, 6, 7): High similarity (>0.7)
- **Kindle generations**: High similarity (>0.7)
- **Cross-category** (phone vs laptop): Low similarity (<0.3)
- **Gaming consoles** (PlayStation vs Xbox): Medium similarity (>0.5)

## Generated Data Format

### materials.csv
```csv
material_id,description,material_group,material_type,created_date
IDENT-COMP-000,Premium Steel Assembly,GRP-08,HALB,2020-01-01
```

### bom_items.csv
```csv
bom_id,parent_id,child_id,quantity,level,position
BOM-IDENT-PROD-A,IDENT-PROD-A,IDENT-COMP-000,4.24,1,0
```

### ground_truth.csv
```csv
material_a,material_b,expected_similarity,relationship_type,notes
IDENT-PROD-A,IDENT-PROD-B,1.0,identical,Identical component sets
```

## Floating-Point Tolerance

For similarity score comparisons, use `ROUND(value, 4)` to handle floating-point precision:

```sql
SELECT ROUND(similarity_score, 4) FROM ...
```

## SQLLogicTest Reference

DuckDB uses [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html) for testing.

### Type Indicators
- `R` - REAL/DOUBLE (for similarity scores)
- `I` - INTEGER
- `T` - TEXT/VARCHAR

### Result Sorting
Use `rowsort` when result order is non-deterministic:
```sql
query RT rowsort
SELECT material_id, similarity FROM results;
----
PROD-A	0.5
PROD-B	0.3
```

### Error Testing
```sql
statement error
SELECT invalid_function();
----
expected error substring
```

## Adding New Tests

1. Create a new `.test` file in the appropriate directory based on functionality
2. Use the standard header format:
   ```sql
   # name: test/sql/functions/my_function.test
   # description: Description of what this test validates
   # tier: tier1  # tier1, tier2, or tier3
   # group: [functions]
   
   require anofox_similarity
   ```
3. Load test data from CSV files
4. Write test queries with expected results
5. Run `make test` to validate

## CI/CD Integration

### Every Commit (Tier 1)
- Synthetic data tests run automatically
- All edge case tests run
- Function tests run
- Algorithm tests run

### Nightly/Release (Tier 2)
- Figshare dataset downloaded and cached
- Product family clustering validated
- Cross-category similarity tested
- Accuracy metrics computed

### Local Only (Tier 3)
- SAP private data never leaves local environment
- Results anonymized before sharing
- Production validation tests run locally

CI/CD filters tests by the `# tier:` metadata in test file headers, not by directory structure.
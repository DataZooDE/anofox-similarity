# AdventureWorks Tutorial Guides

Learn to use **anofox-similarity** for product similarity analysis using Microsoft's AdventureWorks bicycle manufacturing dataset.

## About the Dataset

AdventureWorks simulates a bicycle manufacturing company with:

| Category | Count | Examples |
|----------|-------|----------|
| **Finished Bicycles** | 240 | Road-150 Red 44, Mountain-500 Silver 40, Touring-3000 Blue 50 |
| **Subassemblies** | 27 | Frames, wheels, handlebars, seat assemblies |
| **Raw Materials** | 58 | Metals, tires, fasteners, coatings |
| **BOM Relationships** | 2,576 | Parent-child component links across 4 levels |
| **Ground Truth Pairs** | 100 | Labeled similarity pairs for validation |

**Key characteristics:**
- **Variant families**: Same bike model in different sizes (44/48/52/56/62 cm frames)
- **Quality tiers**: HL (high-end), ML (mid-range), LL (budget) components
- **4-level hierarchy**: Bicycle → Frame → Tubing → Metal sheets

## Quick Setup

```sql
-- Install and load extension
INSTALL anofox_similarity FROM community;
LOAD anofox_similarity;

-- Load AdventureWorks data
CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');

CREATE TABLE ground_truth AS
SELECT * FROM read_csv_auto('test/data/adventureworks/ground_truth.csv.gz');

-- Verify data loaded correctly
SELECT COUNT(*) AS materials FROM materials;        -- 325
SELECT COUNT(*) AS bom_edges FROM bom_items;        -- 2576
SELECT COUNT(*) AS truth_pairs FROM ground_truth;   -- 100
```

## Available Guides

| Guide | Business Question | Key Functions |
|-------|-------------------|---------------|
| [01 - Cold-Start Forecasting](01_cold_start_forecasting.md) | "How do I forecast a new bike with no history?" | `cold_start_analogs()` |
| [02 - Product Families](02_product_families.md) | "Which bikes are variants of each other?" | `find_similar_materials_jaccard()` |
| [03 - BOM Navigation](03_bom_navigation.md) | "What components are in this bike?" | `bom_explosion_multilevel()`, `bom_where_used()` |
| [04 - Quality Tiers](04_quality_tiers.md) | "What if we substitute HL with ML parts?" | `jaccard_similarity()`, `wl_kernel_similarity()` |
| [05 - Accuracy Validation](05_accuracy_validation.md) | "Is the algorithm working correctly?" | Accuracy metrics with ground truth |

## Sample Products

Here are some key products referenced in the guides:

```sql
-- Road-150 family (5 size variants, 13/15 components shared)
SELECT material_id, description FROM materials
WHERE material_id IN ('AW-749', 'AW-750', 'AW-751', 'AW-752', 'AW-753');
```

| ID | Description |
|----|-------------|
| AW-749 | Road-150 Red, 62 |
| AW-750 | Road-150 Red, 44 |
| AW-751 | Road-150 Red, 48 |
| AW-752 | Road-150 Red, 52 |
| AW-753 | Road-150 Red, 56 |

These bikes share all components except the frame (which differs by size), giving them 0.87 Jaccard similarity.

## Prerequisites

- DuckDB 1.0.0+
- anofox-similarity extension installed
- AdventureWorks test data (included in repository at `test/data/adventureworks/`)

## Next Steps

Start with [Guide 01: Cold-Start Forecasting](01_cold_start_forecasting.md) - the primary use case for product similarity in demand planning.

# SAP Tutorial Guides

Learn to use **anofox-similarity** with SAP ERP data using standard SAP table structures (MARA, MAKT, MAST, STKO, STPO).

## About the Dataset

This guide series uses real SAP manufacturing data:

| SAP Table | Records | Description |
|-----------|---------|-------------|
| **MARA** | 1,023,632 | Material Master (material IDs, types, groups) |
| **MAKT** | 7,352,217 | Material Descriptions (multi-language) |
| **MAST** | 614,505 | BOM Assignment (links materials to BOMs) |
| **STKO** | 1,046,638 | BOM Headers (versioning, validity dates) |
| **STPO** | 14,079,161 | BOM Items (component relationships) |

**Key characteristics:**
- **Enterprise scale**: 1M+ materials, 14M+ BOM relationships
- **Alternative BOMs**: Multiple manufacturing approaches per product (01=primary, 02/03=alternates)
- **Date versioning**: Point-in-time BOM snapshots with validity dates
- **Multi-language**: Material descriptions in EN, DE, FR, etc.
- **SAP standard**: Uses native SAP field names (matnr, mtart, matkl, etc.)

## Quick Setup

```sql
-- Install and load extension
INSTALL anofox_similarity FROM community;
LOAD anofox_similarity;

-- Load SAP tables from parquet files
CREATE TABLE sap_mara AS SELECT * FROM read_parquet('test/data/sap/mara.parquet');
CREATE TABLE sap_makt AS SELECT * FROM read_parquet('test/data/sap/makt.parquet');
CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');

-- Verify data loaded correctly
SELECT 'mara' AS tbl, COUNT(*) AS rows FROM sap_mara
UNION ALL SELECT 'makt', COUNT(*) FROM sap_makt
UNION ALL SELECT 'mast', COUNT(*) FROM sap_mast
UNION ALL SELECT 'stko', COUNT(*) FROM sap_stko
UNION ALL SELECT 'stpo', COUNT(*) FROM sap_stpo;
```

## Available Guides

| Guide | Business Question | Key Functions |
|-------|-------------------|---------------|
| [01 - Data Transformation](01_data_transformation.md) | "How do I convert SAP tables for analysis?" | `sap_to_materials()`, `sap_to_bom_items()` |
| [02 - Alternative BOMs](02_alternative_boms.md) | "How do we compare different manufacturing approaches?" | Alternative BOM loading, Jaccard comparison |
| [03 - Point-in-Time Analysis](03_point_in_time_analysis.md) | "How has our BOM changed over time?" | `reference_date` parameter, change tracking |
| [04 - Similarity at Scale](04_similarity_at_scale.md) | "How do I search 1M+ materials efficiently?" | HNSW indexing, embeddings |
| [05 - Multi-Language Descriptions](05_multilanguage_descriptions.md) | "How do I work with multi-language descriptions?" | Language filtering, MAKT joins |

## SAP Table Structure

```
MARA (Material Master)
├── matnr: Material number (18 chars, zero-padded)
├── mtart: Material type (FERT=Finished, HALB=Semi-finished, ROH=Raw)
├── matkl: Material group
├── ersda: Creation date (YYYYMMDD)
└── lvorm: Deletion flag

MAKT (Descriptions)
├── matnr: Material number
├── spras: Language key (E=English, D=German, F=French)
└── maktx: Description text

MAST (BOM Assignment)
├── matnr: Parent material
├── werks: Plant
├── stlan: BOM usage
└── stlnr: BOM number

STKO (BOM Header)
├── stlnr: BOM number
├── stlal: Alternative (01, 02, 03)
├── datuv: Valid-from date
└── bmeng: Base quantity

STPO (BOM Items)
├── stlnr: BOM number
├── idnrk: Component material
├── menge: Quantity
└── meins: Unit of measure
```

## Key Differences from AdventureWorks

| Feature | AdventureWorks | SAP |
|---------|---------------|-----|
| **Scale** | 325 materials | 1,023,632 materials |
| **BOM edges** | 2,576 | 14,079,161 |
| **ETL needed** | No (universal schema) | Yes (SAP → universal) |
| **Alternative BOMs** | No | Yes (01, 02, 03) |
| **Date versioning** | No | Yes (datuv field) |
| **Multi-language** | No | Yes (MAKT table) |

## Prerequisites

- DuckDB 1.0.0+
- anofox-similarity extension installed
- SAP parquet files (included in repository at `test/data/sap/`)
- ~2GB disk space for full dataset

## Performance Note

The SAP dataset is large. Initial queries may take several seconds. For faster iteration during learning:

```sql
-- Create a sample for testing (50K materials)
CREATE TABLE sap_mara_sample AS
SELECT * FROM sap_mara
WHERE matnr IN (SELECT DISTINCT matnr FROM sap_mast LIMIT 50000);
```

## Next Steps

Start with [Guide 01: Data Transformation](01_data_transformation.md) to learn how to convert SAP tables to the universal schema used by anofox-similarity.

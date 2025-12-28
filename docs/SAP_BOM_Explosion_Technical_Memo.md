# SAP Bill of Materials (BOM) Explosion - Technical Memo

## Executive Summary

This document provides complete technical documentation of the SAP Bill of Materials explosion logic implemented in DBT. The implementation transforms SAP's hierarchical BOM data into a flattened, multi-level structure suitable for analytics. The system processes BOMs up to 20 levels deep using recursive SQL.

---

## 1. SAP Source Tables

### 1.1 Core SAP Tables Used

| Table | SAP Name | Purpose |
|-------|----------|---------|
| **MARA** | Material Master | Core material attributes (type, status, division, etc.) |
| **MAKT** | Material Descriptions | Language-specific material text descriptions |
| **MAST** | BOM Master | Links materials to BOM headers by plant/application |
| **STKO** | BOM Header | BOM version control, validity dates, lock status |
| **STAS** | BOM Item Selection | Active/inactive items with validity periods |
| **STPO** | BOM Item Details | Component positions, quantities, units |

### 1.2 Key SAP Fields

```
MARA Fields:
  matnr     - Material number (18-char SAP format with leading zeros)
  zzut      - Turnover key: 01=Product, 02=Accessory, 11=Product variant
  mstae     - Cross-plant material status (F=Released, A=Phased-out, etc.)
  spart     - SAP division code
  matkl     - Material group
  mtart     - Material type
  labor     - Design office/country code (first 2 chars = country)
  ersda     - Creation date (YYYYMMDD format)

MAST Fields:
  matnr     - Material number
  werks     - Plant code
  stlan     - BOM application type: 2=Production, E=Engineering, V=Variant
  stlnr     - BOM number (internal)
  stlal     - Alternative BOM (01=primary)

STKO Fields:
  stlty     - BOM category: M=Material BOM
  stlnr     - BOM number
  stlal     - Alternative BOM
  datuv     - Valid-from date
  lkenz     - Lock indicator (null=active, true=locked)

STAS Fields:
  stlkn     - BOM item node number
  stasz     - Selection parameter
  datuv     - Valid-from date
  lkenz     - Lock indicator

STPO Fields:
  stlkn     - BOM item node
  posnr     - Position number
  postp     - Position type
  idnrk     - Component material number
  rekrs     - Recursive/phantom flag (true=skip in recursion)
  menge     - Quantity
  meins     - Unit of measure
```

---

## 2. Single-Level BOM Extraction

### 2.1 Purpose
Extracts direct parent-component relationships (one level deep). Each row represents one component within its parent's BOM.

### 2.2 Architectural Role

**This model is the foundation for multi-level expansion - it is NOT redundant.**

The single-level model serves as a **pre-computed edge table** that the multi-level recursive query joins against at each iteration. Without it, the recursive query would need to:

1. Rebuild all 10 CTEs and 6-way joins at every recursion depth (up to 20×)
2. Recalculate date validity via LEAD() window function repeatedly
3. Recompute `is_assembly` and `is_active_assembly` flags at each level

**Performance impact**: Pre-computing single-level converts O(n×depth) join operations into O(1) lookups during recursion.

**Independent use cases**:
- Direct component queries without full BOM explosion
- Single-level cost rollups
- BOM validation and auditing
- Component usage analysis

### 2.3 Complete SQL Logic

```sql
-- Step 1: Get English material descriptions
WITH cte__sap__pg1__makt AS (
    SELECT * FROM sap__pg1__makt WHERE language_code = 'EN'
),

-- Step 2: Format material master with parsed dates
cte__sap__pg1__mara__formating AS (
    SELECT
        mara.* EXCEPT (ersda),
        PARSE_DATE('%Y%m%d', mara.ersda) AS ersda
    FROM sap__pg1__mara AS mara
),

-- Step 3: Filter BOM master for relevant application types
cte__bom__dim__mast AS (
    SELECT
        mast.mandt,
        mast.matnr,
        makt.maktx,
        CASE WHEN mast.werks IS NULL THEN 'XX00' ELSE mast.werks END AS werks,
        mast.stlan,
        mast.stlnr,
        mast.stlal
    FROM sap__pg1__mast mast
    INNER JOIN cte__sap__pg1__makt makt ON (mast.matnr = makt.matnr)
    WHERE mast.stlan IN ('2', 'E', 'V')  -- Production, Engineering, Variant BOMs
      AND mast.stlal = '01'              -- Primary alternative only
),

-- Step 4: Get BOM headers for Material BOMs only
cte__sap__pg1__stko__material_bom_category AS (
    SELECT
        mandt,
        stlty,
        stlnr,
        stlal,
        CASE WHEN lkenz IS NULL THEN false ELSE true END AS lkenz,
        datuv
    FROM sap__pg1__stko
    WHERE stlty = 'M'      -- Material BOM type only
      AND stlal = '01'     -- Primary alternative
),

-- Step 5: Calculate validity date ranges using LEAD() window function
cte__sap__pg1__stko__get_date_to AS (
    SELECT
        *,
        -- SAP CS03 logic: next datuv becomes current record's end date
        LEAD(datuv) OVER (
            PARTITION BY stlty, stlnr, stlal
            ORDER BY stlty, stlnr, stlal, lkenz
        ) AS date_to
    FROM cte__sap__pg1__stko__material_bom_category
),

-- Step 6: Assign default end date for current/latest records
cte__sap__pg1__stko__assign_default_date_date_to AS (
    SELECT
        * EXCEPT (date_to),
        CASE WHEN date_to IS NULL THEN '9999-12-31' ELSE date_to END AS date_to
    FROM cte__sap__pg1__stko__get_date_to
),

-- Step 7: Filter to active, currently valid BOMs
cte__bom__dim__stko AS (
    SELECT *
    FROM cte__sap__pg1__stko__assign_default_date_date_to
    WHERE lkenz = false                      -- Not locked
      AND date_to >= CURRENT_DATE("UTC")     -- Still valid
),

-- Step 8: Same date range logic for BOM items (STAS)
cte__sap__pg1__stas__material_bom_category AS (
    SELECT
        mandt, stlty, stlnr, stlal, stlkn, stasz, datuv,
        CASE WHEN lkenz IS NULL THEN false ELSE true END AS lkenz
    FROM sap__pg1__stas
    WHERE stlty = 'M'
),

cte__sap__pg1__stas__get_date_to AS (
    SELECT
        *,
        LEAD(datuv) OVER (
            PARTITION BY stlty, stlnr, stlal, stlkn
            ORDER BY stlty, stlnr, stlal, stlkn, lkenz
        ) AS date_to
    FROM cte__sap__pg1__stas__material_bom_category
),

cte__sap__pg1__stas__assign_default_date_date_to AS (
    SELECT
        * EXCEPT (date_to),
        CASE WHEN date_to IS NULL THEN '9999-12-31' ELSE date_to END AS date_to
    FROM cte__sap__pg1__stas__get_date_to
),

cte__bom_dim_stas AS (
    SELECT *
    FROM cte__sap__pg1__stas__assign_default_date_date_to
    WHERE lkenz = false AND date_to >= CURRENT_DATE("UTC")
),

-- Step 9: Get component details from STPO
cte__bom__dim__stpo AS (
    SELECT
        stpo.mandt,
        stlnr,
        stlty,
        stlkn,
        posnr,
        postp,
        idnrk,
        makt.maktx AS idnrk_maktx,
        CASE WHEN rekrs IS NULL THEN false ELSE true END AS rekrs,
        menge,
        meins,
        datuv
    FROM sap__pg1__stpo stpo
    INNER JOIN cte__sap__pg1__makt makt ON (stpo.idnrk = makt.matnr)
    WHERE stlty = 'M'
),

-- Step 10: Assemble single-level BOM with all joins
cte_mart_dim_bill_of_materials__single_level AS (
    SELECT
        -- Parent material attributes
        mast.matnr AS mast_matnr,
        mast.maktx AS mast_maktx,
        mara.zzut AS mara_zzut,
        mara.mstae AS mara_mstae,
        mara.spart AS mara_spart,
        mara.ersda AS mara_ersda,
        mara.matkl AS mara_matkl,
        mara.mtart AS mara_mtart,
        mara.labor AS mara_labor,

        -- BOM structure
        mast.werks AS mast_werks,
        mast.stlan AS mast_stlan,
        mast.stlnr AS mast_stlnr,
        stpo.stlty AS stpo_stlty,
        stpo.posnr AS stpo_posnr,
        stpo.postp AS stpo_postp,

        -- Component attributes
        stpo.idnrk AS stpo_idnrk,
        stpo.idnrk_maktx AS idnrk_maktx,
        mara_idnrk.zzut AS idnrk_zzut,
        mara_idnrk.mstae AS idnrk_mstae,
        mara_idnrk.spart AS idnrk_spart,
        mara_idnrk.ersda AS idnrk_ersda,
        mara_idnrk.matkl AS idnrk_matkl,
        mara_idnrk.mtart AS idnrk_mtart,
        mara_idnrk.labor AS idnrk_labor,

        -- Component's BOM (for recursion detection)
        CASE
            WHEN mast_idnrk.stlnr IS NULL THEN '00000000'
            ELSE mast_idnrk.stlnr
        END AS idnrk_stlnr,

        -- Quantity info
        stpo.rekrs AS stpo_rekrs,
        stpo.menge AS stpo_menge,
        stpo.meins AS stpo_meins,

        -- Assembly flags (critical for recursion)
        CASE WHEN mast_idnrk.matnr IS NULL THEN false ELSE true END AS is_assembly,
        CASE WHEN mast_idnrk_stko.stlnr IS NULL THEN false ELSE true END AS is_active_assembly

    FROM cte__bom_dim_stas stas

    -- Join to get component details
    INNER JOIN cte__bom__dim__stpo stpo ON (
        stpo.mandt = stas.mandt
        AND stpo.stlty = stas.stlty
        AND stpo.stlnr = stas.stlnr
        AND stpo.stlkn = stas.stlkn
    )

    -- Join to BOM header
    INNER JOIN cte__bom__dim__stko stko ON (
        stko.mandt = stas.mandt
        AND stko.stlty = stas.stlty
        AND stko.stlnr = stas.stlnr
        AND stko.stlal = stas.stlal
    )

    -- Join to get parent material info
    INNER JOIN cte__bom__dim__mast mast ON (
        mast.mandt = stko.mandt
        AND mast.stlnr = stko.stlnr
        AND mast.stlal = stko.stlal
    )

    -- Join parent MARA
    INNER JOIN cte__sap__pg1__mara__formating AS mara ON (
        mara.mandt = mast.mandt
        AND mara.matnr = mast.matnr
    )

    -- Check if component has its own BOM (for is_assembly flag)
    LEFT JOIN cte__bom__dim__mast mast_idnrk ON (
        mast_idnrk.mandt = stpo.mandt
        AND mast_idnrk.matnr = stpo.idnrk
        AND mast_idnrk.werks = mast.werks    -- Same plant
        AND mast_idnrk.stlan = mast.stlan    -- Same BOM application
    )

    -- Check if component's BOM is active (for is_active_assembly)
    LEFT JOIN cte__bom__dim__stko mast_idnrk_stko ON (
        mast_idnrk_stko.mandt = stpo.mandt
        AND mast_idnrk_stko.stlty = stpo.stlty
        AND mast_idnrk_stko.stlnr = mast_idnrk.stlnr
        AND mast_idnrk_stko.stlal = '01'
    )

    -- Join component MARA
    LEFT JOIN cte__sap__pg1__mara__formating AS mara_idnrk ON (
        mara_idnrk.mandt = stpo.mandt
        AND mara_idnrk.matnr = stpo.idnrk
    )
)

SELECT * FROM cte_mart_dim_bill_of_materials__single_level
```

### 2.4 Output Columns

| Column | Description |
|--------|-------------|
| `mast_matnr` | Parent material number |
| `mast_maktx` | Parent description |
| `mast_werks` | Plant code |
| `mast_stlan` | BOM application type |
| `stpo_idnrk` | Component material number |
| `stpo_menge` | Component quantity |
| `stpo_meins` | Unit of measure |
| `stpo_rekrs` | Phantom/recursive flag |
| `is_assembly` | Component has a BOM (true/false) |
| `is_active_assembly` | Component has ACTIVE BOM (true/false) |

---

## 3. Collector (Top-Level Products)

### 3.1 Purpose
Creates a dimension of all materials that serve as BOM headers (products). These are the starting points for multi-level expansion.

### 3.2 SQL Logic

```sql
WITH cte__sap__pg1__makt AS (
    SELECT * FROM sap__pg1__makt WHERE language_code = 'EN'
),

cte__sap__pg1__mara__formating AS (
    SELECT
        mara.* EXCEPT (ersda),
        PARSE_DATE('%Y%m%d', mara.ersda) AS ersda
    FROM sap__pg1__mara mara
),

-- Rename columns with 'collector' prefix
cte_sap__pg1__mara AS (
    SELECT
        matnr AS matnr_collector,
        zzut AS matnr_collector_zzut,
        mstae AS matnr_collector_mstae,
        spart AS matnr_collector_spart,
        ersda AS matnr_collector_ersda,
        matkl AS matnr_collector_matkl,
        mtart AS matnr_collector_mtart,
        labor AS matnr_collector_labor
    FROM cte__sap__pg1__mara__formating
),

-- Get materials that have BOMs (stlan='2' = Production)
cte_sap__pg1__mast AS (
    SELECT
        matnr,
        CASE WHEN werks IS NULL THEN 'XX00' ELSE werks END AS matnr_collector_werks,
        stlal AS matnr_collector_stlal,
        stlan AS matnr_collector_stlan
    FROM sap__pg1__mast
    WHERE stlan = '2'  -- Production BOMs only
),

-- Join to get all materials, even those without BOMs
cte_join_mara_mast AS (
    SELECT *
    FROM cte_sap__pg1__mara mara
    LEFT JOIN cte_sap__pg1__mast mast ON (mast.matnr = mara.matnr_collector)
),

-- Apply null defaults and get descriptions
cte_mart_dim_bom_macl AS (
    SELECT
        * EXCEPT (matnr, matnr_collector_werks, matnr_collector_stlal,
                  matnr_collector_stlan),
        CASE WHEN matnr_collector_werks IS NULL THEN 'XX00'
             ELSE matnr_collector_werks END AS matnr_collector_werks,
        CASE WHEN matnr_collector_stlal IS NULL THEN 'X0'
             ELSE matnr_collector_stlal END AS matnr_collector_stlal,
        CASE WHEN matnr_collector_stlan IS NULL THEN 'X'
             ELSE matnr_collector_stlan END AS matnr_collector_stlan,
        makt.maktx AS matnr_collector_maktx
    FROM cte_join_mara_mast macl
    INNER JOIN cte__sap__pg1__makt makt ON (macl.matnr_collector = makt.matnr)
)

SELECT * FROM cte_mart_dim_bom_macl
```

### 3.3 Null Value Defaults

| Field | Default | Meaning |
|-------|---------|---------|
| `werks` | 'XX00' | No specific plant |
| `stlal` | 'X0' | No alternative BOM |
| `stlan` | 'X' | No BOM application |

---

## 4. Multi-Level Recursive Expansion

### 4.1 Purpose
Explodes BOMs recursively up to 20 levels deep, maintaining the hierarchy path from top-level product to leaf components.

### 4.2 Recursive Algorithm

```
ALGORITHM: BOM Multi-Level Expansion
INPUT: Single-level BOM relationships, Collector products
OUTPUT: Flattened multi-level BOM with hierarchy tracking

1. ANCHOR (Level 1):
   - Select products from Collector where zzut IN ('01', '11')
   - Join to Single-Level BOM to get direct components
   - Set iteration=1, nodelevel=1

2. RECURSIVE (Levels 2-20):
   FOR each row in previous iteration WHERE:
     - iteration < 20 (safety limit)
     - is_active_assembly = TRUE (component has active BOM)
     - stpo_rekrs = FALSE (not a phantom assembly)
   DO:
     - Take stpo_idnrk (component) as new parent
     - Join to Single-Level BOM matching:
       - component becomes new parent (stpo_idnrk = mast_matnr)
       - Same plant (matnr_collector_werks = mast_werks)
       - Same BOM application (matnr_collector_stlan = mast_stlan)
     - Increment iteration
     - Increment nodelevel
   END

3. TERMINATION:
   Recursion stops when:
   - iteration >= 20 (max depth reached)
   - is_active_assembly = FALSE (leaf component)
   - stpo_rekrs = TRUE (phantom - skip but continue with children)
```

### 4.3 Complete SQL Implementation

```sql
-- Configuration: Product turnover keys to include
-- lv_zzut = ["01", "11"]  (01=Products, 11=Product variants)
-- lv_nodelevel = 1        (Starting level)

-- Subquery: Single-level BOM data
-- cte_bomsingle_query =
SELECT
    sglv.mast_matnr,
    sglv.mast_maktx,
    sglv.mast_werks,
    sglv.mast_stlan,
    sglv.stpo_posnr,
    sglv.stpo_postp,
    sglv.stpo_idnrk,
    sglv.idnrk_maktx,
    sglv.idnrk_zzut,
    sglv.idnrk_mstae,
    sglv.idnrk_spart,
    sglv.idnrk_ersda,
    sglv.idnrk_matkl,
    sglv.idnrk_mtart,
    sglv.idnrk_labor,
    sglv.stpo_rekrs,
    sglv.stpo_menge,
    sglv.stpo_meins,
    sglv.is_active_assembly
FROM mart_dim_bill_of_materials__single_level sglv
ORDER BY mast_matnr, mast_werks, mast_stlan, stpo_posnr

-- Subquery: Top-level products (collectors)
-- cte_matnr_collector_query =
SELECT
    macl.matnr_collector,
    makt.maktx AS matnr_collector_maktx,
    macl.matnr_collector_werks,
    macl.matnr_collector_stlan
FROM mart_dim_bill_of_materials__collector macl
INNER JOIN sap__pg1__makt makt ON (macl.matnr_collector = makt.matnr)
WHERE matnr_collector_zzut IN ('01', '11')  -- Products only
ORDER BY matnr_collector, matnr_collector_stlal

-- MAIN RECURSIVE QUERY
WITH RECURSIVE cte__bomitems AS (

    -- ANCHOR: Level 1 - Direct components of products
    SELECT
        1 AS iteration,
        1 AS nodelevel,
        matnr_collector.matnr_collector,
        matnr_collector.matnr_collector_maktx,
        matnr_collector.matnr_collector_werks,
        matnr_collector.matnr_collector_stlan,
        bomsingle.* EXCEPT (mast_werks, mast_stlan)
    FROM (cte_matnr_collector_query) matnr_collector
    INNER JOIN (cte_bomsingle_query) bomsingle ON (
        matnr_collector.matnr_collector = bomsingle.mast_matnr
        AND matnr_collector.matnr_collector_werks = bomsingle.mast_werks
        AND matnr_collector.matnr_collector_stlan = bomsingle.mast_stlan
    )

    UNION ALL

    -- RECURSIVE: Levels 2-20 - Traverse into sub-assemblies
    (
        SELECT
            iteration,
            nodelevel + 1 AS nodelevel,
            matnr_collector.matnr_collector,
            matnr_collector.matnr_collector_maktx,
            matnr_collector.matnr_collector_werks,
            matnr_collector.matnr_collector_stlan,
            bomsingle.* EXCEPT (mast_werks, mast_stlan)
        FROM (
            -- Get components that are assemblies from previous level
            SELECT
                iteration + 1 AS iteration,
                nodelevel,
                matnr_collector,
                matnr_collector_maktx,
                matnr_collector_werks,
                matnr_collector_stlan,
                stpo_idnrk  -- This component becomes the new parent
            FROM cte__bomitems
            WHERE iteration < 20                  -- Safety limit
              AND is_active_assembly = TRUE       -- Has active BOM
              AND stpo_rekrs = false              -- Not phantom
        ) matnr_collector

        -- Join to get this component's children
        INNER JOIN (cte_bomsingle_query) bomsingle ON (
            matnr_collector.stpo_idnrk = bomsingle.mast_matnr
            AND matnr_collector.matnr_collector_werks = bomsingle.mast_werks
            AND matnr_collector.matnr_collector_stlan = bomsingle.mast_stlan
        )
    )
)

SELECT * FROM cte__bomitems
```

### 4.4 Recursion Control Fields

| Field | Purpose |
|-------|---------|
| `iteration` | Recursion counter (1-20) |
| `nodelevel` | Depth in hierarchy (starts at 1) |
| `matnr_collector` | Top-level product (preserved throughout) |
| `is_active_assembly` | TRUE if component has active BOM to traverse |
| `stpo_rekrs` | TRUE = phantom assembly (skip in recursion) |

### 4.5 Termination Conditions

1. **Max Depth**: `iteration >= 20` - Prevents infinite loops
2. **Leaf Node**: `is_active_assembly = FALSE` - Component has no BOM
3. **Phantom**: `stpo_rekrs = TRUE` - Skip this level

---

## 5. Access Control (Serving Layer)

### 5.1 Purpose
Filters data based on user's governance permissions using session context.

### 5.2 SQL Implementation

```sql
WITH cte_source AS (
    SELECT *,
    LEFT(mara_labor, 2) AS country_design_office  -- Extract country from labor code
    FROM mart_dim_bill_of_materials__single_level
),

-- Get user's allowed countries from governance table
cte_restricted_country_list AS (
    SELECT DISTINCT country_code
    FROM governance
    WHERE user_id = SESSION_USER()
),

-- Check if user has all-access (AKW = All-Knowing Warehouse)
cte_restricted_to_akw AS (
    SELECT is_akw
    FROM governance
    WHERE user_id = SESSION_USER()
),

-- Normalize country codes (MBS -> DE)
normalization AS (
    SELECT * EXCEPT (country_design_office),
    CASE
        WHEN country_design_office = 'MBS' THEN 'DE'
        WHEN country_design_office IS NULL THEN 'DE'
        ELSE country_design_office
    END AS country_design_office
    FROM cte_source
),

-- Apply access filter
cte_final AS (
    SELECT *
    FROM normalization
    WHERE country_design_office IN (SELECT country_code FROM cte_restricted_country_list)
       OR TRUE IN (SELECT is_akw FROM cte_restricted_to_akw)  -- AKW bypasses filter
)

SELECT * FROM cte_final
```

### 5.3 Access Rules

| Condition | Access |
|-----------|--------|
| `is_akw = TRUE` | Full access to all materials |
| Country in user's list | Access to materials from that country |
| MBS country code | Normalized to DE |
| NULL country | Defaults to DE |

---

## 6. PowerBI Application Layer

### 6.1 Purpose
Creates a unified, denormalized view combining three data sources for PowerBI analytics.

### 6.2 Data Sources (Union)

| Source CTE | Content | Filter |
|------------|---------|--------|
| `cte__products` | BOM Headers | Collector where zzut IN ('01','11') |
| `cte__components` | Multi-level items | All expanded BOM components |
| `cte__parts` | Orphan parts | Materials NOT in any BOM |

### 6.3 Part Category Mapping (Teilegruppe)

```sql
CASE
    WHEN zzut IN ('01', '1', '11') THEN 'Products'
    WHEN zzut IN ('2', '02', '22', '5', '05', '28') THEN 'Accessories'
    WHEN is_active_assembly = TRUE THEN 'Assembly'
    WHEN LOWER(matkl) LIKE 'r%' THEN 'Raw Material'
    ELSE 'Component'
END AS component__teilegruppe
```

### 6.4 Material Status Mapping

```sql
-- Generic code
CASE
    WHEN mstae IN ('', 'P0') OR mstae LIKE '0%' OR mstae LIKE 'V%' THEN 'p'
    WHEN mstae LIKE 'F%' THEN 'f'
    WHEN mstae LIKE 'A%' OR mstae LIKE 'G%' OR mstae LIKE 'U%' THEN 'a'
    WHEN mstae LIKE 'I%' THEN 'i'
    ELSE 'x'
END AS component__cross_plant_material_status_code_gen

-- Short description
CASE
    WHEN mstae IN ('', 'P0') OR mstae LIKE '0%' OR mstae LIKE 'V%' THEN 'Before release'
    WHEN mstae LIKE 'F%' THEN 'Released'
    WHEN mstae LIKE 'A%' OR mstae LIKE 'G%' OR mstae LIKE 'U%' THEN 'Phased-out'
    WHEN mstae LIKE 'I%' THEN 'Invalid'
    ELSE 'None'
END AS component__cross_plant_material_status_code_short
```

---

## 7. Complete Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                       SAP SOURCE TABLES                         │
├─────────────────────────────────────────────────────────────────┤
│  MARA (Material Master)  ─────────────────────────────┐         │
│  MAKT (Descriptions)     ─────────────────────────┐   │         │
│  MAST (BOM Master)       ────────────────────┐    │   │         │
│  STKO (BOM Header)       ───────────────┐    │    │   │         │
│  STAS (BOM Item Status)  ──────────┐    │    │    │   │         │
│  STPO (BOM Items)        ─────┐    │    │    │    │   │         │
└───────────────────────────────┼────┼────┼────┼────┼───┼─────────┘
                                │    │    │    │    │   │
                                ▼    ▼    ▼    ▼    ▼   ▼
┌─────────────────────────────────────────────────────────────────┐
│                    02_MARTS LAYER                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │     mart_dim_bill_of_materials__single_level             │   │
│  │     ─────────────────────────────────────────            │   │
│  │     • Joins STAS→STPO→STKO→MAST→MARA                     │   │
│  │     • Date validity: LEAD() for valid_to calculation     │   │
│  │     • Filters: stlan IN ('2','E','V'), stlal='01'        │   │
│  │     • Output: Parent→Component single-level relationships│   │
│  │     • Key fields: is_assembly, is_active_assembly        │   │
│  └──────────────────────────────────────────────────────────┘   │
│                             │                                   │
│  ┌──────────────────────────┼───────────────────────────────┐   │
│  │     mart_dim_bill_of_materials__collector                │   │
│  │     ───────────────────────────────────────              │   │
│  │     • All materials from MARA                            │   │
│  │     • LEFT JOIN to MAST where stlan='2'                  │   │
│  │     • Null defaults: werks=XX00, stlal=X0, etc.          │   │
│  │     • Output: Product dimension table                    │   │
│  └──────────────────────────┼───────────────────────────────┘   │
│                             │                                   │
│  ┌──────────────────────────▼───────────────────────────────┐   │
│  │     mart_dim_bill_of_materials__multi_level              │   │
│  │     ──────────────────────────────────────               │   │
│  │     • WITH RECURSIVE cte__bomitems                       │   │
│  │     • ANCHOR: Collector (zzut IN '01','11')              │   │
│  │         → Single-level components                        │   │
│  │     • RECURSIVE: WHERE iteration<20                      │   │
│  │         AND is_active_assembly=TRUE                      │   │
│  │         AND stpo_rekrs=FALSE                             │   │
│  │     • Output: Flattened hierarchy with nodelevel         │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                    03_SERVING LAYER                             │
├─────────────────────────────────────────────────────────────────┤
│     Views with SESSION_USER() access control                    │
│     • Extract country from labor field (LEFT 2 chars)           │
│     • Filter by governance.country_code OR is_akw=TRUE          │
│     • Normalize: MBS→DE, NULL→DE                                │
└─────────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                    05_APPLICATION LAYER                         │
├─────────────────────────────────────────────────────────────────┤
│     powerbi__hir__bill_of_materials                             │
│     ─────────────────────────────────                           │
│     UNION ALL of:                                               │
│       1. cte__products (BOM headers from collector)             │
│       2. cte__components (multi-level items)                    │
│       3. cte__parts (orphan materials not in any BOM)           │
│                                                                 │
│     Transformations:                                            │
│       • teilegruppe mapping (Products/Accessories/etc.)         │
│       • mstae status mapping (Released/Phased-out/etc.)         │
│       • Column renaming with component__ prefix                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 8. Files Reference

| Layer | File | Purpose |
|-------|------|---------|
| 02_marts | `mart_dim_bill_of_materials__single_level.sql` | Single-level BOM extraction |
| 02_marts | `mart_dim_bill_of_materials__collector.sql` | Product dimension |
| 02_marts | `mart_dim_bill_of_materials__multi_level.sql` | Recursive expansion |
| 03_serving | `dim_bill_of_materials__single_level.sql` | Access-controlled view |
| 03_serving | `dim_bill_of_materials__collector.sql` | Access-controlled view |
| 03_serving | `dim_bill_of_materials_multi_level.sql` | Access-controlled view |
| 05_application | `powerbi__hir__bill_of_materials.sql` | PowerBI consumption |

---

## 9. Reimplementation Checklist

To reimplement this logic:

1. **Source Tables**: Ensure access to MARA, MAKT, MAST, STKO, STAS, STPO
2. **Date Validity**: Implement LEAD() window function for valid_to calculation
3. **Single-Level**: Build the 6-way join with assembly flags
4. **Collector**: Create product dimension with null defaults
5. **Multi-Level**: Implement WITH RECURSIVE with iteration/nodelevel tracking
6. **Termination**: Enforce iteration<20, is_active_assembly, rekrs checks
7. **Access Control**: Implement governance filter with SESSION_USER()
8. **Final Union**: Combine products + components + orphan parts

---

*Document generated from DBT model analysis*
*Source: lakehouse-dbt-model/dbt/models/*

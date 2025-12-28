# Microsoft Dynamics 365 Finance & Operations: Bill of Materials (BOM) Explosion Technical Memo

## Executive Summary

This document provides a comprehensive technical specification for extracting and exploding Bill of Materials (BOM) data from Microsoft Dynamics 365 Finance & Operations (D365 F&O). The approach mirrors the SAP BOM explosion pattern: extract source tables via OData to a local database, build a pre-computed single-level edge table, then use recursive SQL CTE to explode multi-level BOMs up to 20 levels deep.

---

## 1. Source Tables Overview

### 1.1 Core D365 F&O BOM Tables

| Table | D365 Name | Purpose |
|-------|-----------|---------|
| **InventTable** | Product Master | Core item attributes (type, group, status) |
| **BOMTable** | BOM Header | BOM definition with approval status |
| **BOMVersion** | BOM Version | Links items to BOMs with validity dates (many-to-many) |
| **BOM** | BOM Lines | Component positions, quantities, units |

### 1.2 Key D365 F&O Fields

```
InventTable Fields:
  ItemId        - Item number (primary key)
  ItemName      - Item description
  ItemType      - Item type (0=Item, 1=Service, 2=BOM)
  ItemGroupId   - Item group code
  ProdPoolId    - Production pool
  DataAreaId    - Legal entity (company)

BOMVersion Fields:
  BOMId         - Reference to BOMTable
  ItemId        - Finished/parent item number
  Active        - Is this version active? (boolean)
  Approved      - Is this version approved? (boolean)
  Approver      - User who approved
  FromDate      - Validity start date
  ToDate        - Validity end date
  FromQty       - Minimum quantity threshold
  Name          - Version description
  DataAreaId    - Legal entity

BOMTable Fields:
  BOMId         - BOM identifier (primary key)
  Name          - BOM description
  SiteId        - Site restriction
  Approved      - Approval status
  Approver      - Approver user
  DataAreaId    - Legal entity

BOM (Lines) Fields:
  BOMId         - Reference to BOMTable header
  ItemId        - Component item number
  LineNum       - Line number for ordering
  Position      - Position identifier
  BOMQty        - Quantity required
  BOMQtySerie   - Quantity for series production (divisor)
  UnitId        - Unit of measure
  FromDate      - Line validity start date
  ToDate        - Line validity end date
  BOMType       - Type: 0=Item, 1=Phantom, 2=Pegged, 3=Vendor
  BOMConsump    - Consumption: 0=Variable, 1=Constant, 2=Pieces
  OprNum        - Operation number (routing link)
  InventDimId   - Inventory dimensions reference
  ScrapConst    - Constant scrap quantity
  ScrapVar      - Variable scrap percentage
  DataAreaId    - Legal entity
  RecId         - Unique record identifier
```

### 1.3 Table Relationships (Mapped to SAP Equivalents)

```
┌─────────────────┐
│   InventTable   │  (Product Master - like SAP MARA)
│     ItemId      │
└────────┬────────┘
         │ 1:N
         ▼
┌─────────────────┐
│   BOMVersion    │  (Item ↔ BOM Link - like SAP MAST)
│ ItemId, BOMId   │
│ Active, FromDate│
│ ToDate, Approved│
└────────┬────────┘
         │ N:1
         ▼
┌─────────────────┐
│    BOMTable     │  (BOM Header - like SAP STKO)
│     BOMId       │
│ SiteId, Approved│
└────────┬────────┘
         │ 1:N
         ▼
┌─────────────────┐
│      BOM        │  (BOM Lines - like SAP STPO)
│ BOMId, ItemId   │
│ BOMQty, UnitId  │
│ Position, LineNum│
└─────────────────┘
```

---

## 2. OData Extraction to Local Database

### 2.1 OData Data Entities

| OData Entity | Source Table | Key Fields |
|--------------|--------------|------------|
| `ReleasedProductsV2` | InventTable | ItemNumber, ProductType, ItemModelGroupId |
| `BillOfMaterialsHeadersV2` | BOMTable | BOMId, SiteId, IsApproved |
| `BillOfMaterialsVersionsV2` | BOMVersion | BOMId, ItemNumber, IsActive, ValidFromDate, ValidToDate |
| `BillOfMaterialsLinesV2` | BOM | BOMId, ItemNumber, Quantity, UnitSymbol, LineNumber |

### 2.2 OData Endpoint Configuration

**Service Root URL:**
```
https://[your-environment].operations.dynamics.com/data/
```

**Metadata Endpoint (for schema discovery):**
```
https://[your-environment].operations.dynamics.com/data/$metadata
```

### 2.3 Authentication (OAuth 2.0)

```http
POST https://login.microsoftonline.com/{tenant-id}/oauth2/token
Content-Type: application/x-www-form-urlencoded

grant_type=client_credentials
&client_id={application-id}
&client_secret={client-secret}
&resource=https://{your-environment}.operations.dynamics.com
```

### 2.4 Extraction Queries

**Extract BOM Versions (with cross-company):**
```
GET /data/BillOfMaterialsVersionsV2?cross-company=true
    &$select=BOMId,ItemNumber,IsActive,IsApproved,ValidFromDate,ValidToDate,dataAreaId
    &$filter=IsActive eq true
```

**Extract BOM Lines:**
```
GET /data/BillOfMaterialsLinesV2?cross-company=true
    &$select=BOMId,ItemNumber,Quantity,QuantityDenominator,UnitSymbol,LineNumber,
             Position,BOMLineType,ValidFromDate,ValidToDate,dataAreaId
```

**Pagination (max 10,000 per page):**
```
GET /data/BillOfMaterialsLinesV2?$top=10000&$skip=0
GET /data/BillOfMaterialsLinesV2?$top=10000&$skip=10000
...
```

### 2.5 Local Staging Tables

After OData extraction, land data in staging tables:

```sql
-- Staging: BOM Versions
CREATE TABLE stg_d365__bom_versions (
    bom_id              VARCHAR(20),
    item_id             VARCHAR(20),
    is_active           BOOLEAN,
    is_approved         BOOLEAN,
    valid_from_date     DATE,
    valid_to_date       DATE,
    from_qty            DECIMAL(18,6),
    data_area_id        VARCHAR(4),
    _extracted_at       TIMESTAMP
);

-- Staging: BOM Lines
CREATE TABLE stg_d365__bom_lines (
    bom_id              VARCHAR(20),
    item_id             VARCHAR(20),
    line_num            DECIMAL(10,2),
    position            VARCHAR(10),
    bom_qty             DECIMAL(18,6),
    bom_qty_serie       DECIMAL(18,6),
    unit_id             VARCHAR(10),
    bom_type            INT,           -- 0=Item, 1=Phantom, 2=Pegged, 3=Vendor
    valid_from_date     DATE,
    valid_to_date       DATE,
    data_area_id        VARCHAR(4),
    _extracted_at       TIMESTAMP
);

-- Staging: Items (Products)
CREATE TABLE stg_d365__items (
    item_id             VARCHAR(20),
    item_name           VARCHAR(100),
    item_type           INT,
    item_group_id       VARCHAR(10),
    data_area_id        VARCHAR(4),
    _extracted_at       TIMESTAMP
);
```

---

## 3. Single-Level BOM Extraction

### 3.1 Purpose

Extracts direct parent-component relationships (one level deep). Each row represents one component within its parent's BOM.

### 3.2 Architectural Role

**This model is the foundation for multi-level expansion - it is NOT redundant.**

The single-level model serves as a **pre-computed edge table** that the multi-level recursive query joins against at each iteration. Without it, the recursive query would need to:

1. Rebuild all joins and date validity filters at every recursion depth (up to 20×)
2. Recalculate validity window filters repeatedly
3. Recompute `is_assembly` and `is_active_assembly` flags at each level

**Performance impact**: Pre-computing single-level converts O(n×depth) join operations into O(1) lookups during recursion.

**Independent use cases**:
- Direct component queries without full BOM explosion
- Single-level cost rollups
- BOM validation and auditing
- Component usage analysis

### 3.3 Complete SQL Logic

```sql
-- ============================================================
-- SINGLE-LEVEL BOM EXTRACTION
-- Analogous to SAP's mart_dim_bill_of_materials__single_level
-- ============================================================

WITH cte__bom_versions AS (
    -- Filter to active, approved, currently valid BOM versions
    SELECT
        bom_id,
        item_id,
        is_active,
        is_approved,
        valid_from_date,
        valid_to_date,
        data_area_id
    FROM stg_d365__bom_versions
    WHERE is_active = TRUE
      AND is_approved = TRUE
      AND valid_from_date <= CURRENT_DATE
      AND (valid_to_date IS NULL OR valid_to_date >= CURRENT_DATE)
),

cte__bom_lines AS (
    -- Filter to currently valid BOM lines
    SELECT
        bom_id,
        item_id,
        line_num,
        position,
        bom_qty,
        bom_qty_serie,
        unit_id,
        bom_type,
        valid_from_date,
        valid_to_date,
        data_area_id
    FROM stg_d365__bom_lines
    WHERE (valid_from_date IS NULL OR valid_from_date <= CURRENT_DATE)
      AND (valid_to_date IS NULL OR valid_to_date >= CURRENT_DATE)
),

cte__items AS (
    SELECT
        item_id,
        item_name,
        item_type,
        item_group_id,
        data_area_id
    FROM stg_d365__items
),

-- Check which components have their own active BOMs (for recursion detection)
cte__components_with_boms AS (
    SELECT DISTINCT item_id, data_area_id
    FROM cte__bom_versions
),

-- ============================================================
-- MAIN SINGLE-LEVEL JOIN
-- ============================================================
cte__single_level AS (
    SELECT
        -- Parent material attributes
        bv.item_id              AS parent_item_id,
        parent.item_name        AS parent_item_name,
        parent.item_type        AS parent_item_type,
        parent.item_group_id    AS parent_item_group_id,

        -- BOM structure
        bv.bom_id               AS bom_id,
        bl.line_num             AS line_num,
        bl.position             AS position,
        bl.bom_type             AS bom_type,

        -- Component attributes
        bl.item_id              AS component_item_id,
        comp.item_name          AS component_item_name,
        comp.item_type          AS component_item_type,
        comp.item_group_id      AS component_item_group_id,

        -- Quantity info
        bl.bom_qty              AS bom_qty,
        bl.bom_qty_serie        AS bom_qty_serie,
        bl.unit_id              AS unit_id,

        -- Component's BOM reference (for recursion detection)
        COALESCE(comp_bom.item_id, '') AS component_bom_item_id,

        -- Assembly flags (critical for recursion)
        CASE
            WHEN comp_bom.item_id IS NOT NULL THEN TRUE
            ELSE FALSE
        END AS is_assembly,

        CASE
            WHEN comp_bom.item_id IS NOT NULL
             AND bl.bom_type <> 1  -- Not phantom
            THEN TRUE
            ELSE FALSE
        END AS is_active_assembly,

        -- Phantom flag (like SAP's stpo_rekrs)
        CASE
            WHEN bl.bom_type = 1 THEN TRUE
            ELSE FALSE
        END AS is_phantom,

        -- Legal entity
        bv.data_area_id         AS data_area_id

    FROM cte__bom_versions bv

    -- Join to BOM lines
    INNER JOIN cte__bom_lines bl
        ON bv.bom_id = bl.bom_id
       AND bv.data_area_id = bl.data_area_id

    -- Join parent item master
    INNER JOIN cte__items parent
        ON bv.item_id = parent.item_id
       AND bv.data_area_id = parent.data_area_id

    -- Join component item master
    LEFT JOIN cte__items comp
        ON bl.item_id = comp.item_id
       AND bl.data_area_id = comp.data_area_id

    -- Check if component has its own BOM (for is_assembly flag)
    LEFT JOIN cte__components_with_boms comp_bom
        ON bl.item_id = comp_bom.item_id
       AND bl.data_area_id = comp_bom.data_area_id
)

SELECT * FROM cte__single_level
```

### 3.4 Output Columns

| Column | Description |
|--------|-------------|
| `parent_item_id` | Parent/assembly item number |
| `parent_item_name` | Parent description |
| `bom_id` | BOM identifier |
| `component_item_id` | Component material number |
| `bom_qty` | Component quantity per parent |
| `bom_qty_serie` | Series quantity (divisor) |
| `unit_id` | Unit of measure |
| `bom_type` | 0=Item, 1=Phantom, 2=Pegged, 3=Vendor |
| `is_assembly` | Component has a BOM (true/false) |
| `is_active_assembly` | Component has active, non-phantom BOM (true/false) |
| `is_phantom` | Phantom assembly flag (like SAP rekrs) |

---

## 4. Collector (Top-Level Products)

### 4.1 Purpose

Creates a dimension of all materials that serve as BOM headers (products). These are the starting points for multi-level expansion.

### 4.2 SQL Logic

```sql
-- ============================================================
-- COLLECTOR: TOP-LEVEL PRODUCTS
-- Analogous to SAP's mart_dim_bill_of_materials__collector
-- ============================================================

WITH cte__items AS (
    SELECT
        item_id,
        item_name,
        item_type,
        item_group_id,
        data_area_id
    FROM stg_d365__items
),

cte__bom_versions AS (
    SELECT DISTINCT
        item_id,
        data_area_id
    FROM stg_d365__bom_versions
    WHERE is_active = TRUE
      AND is_approved = TRUE
      AND valid_from_date <= CURRENT_DATE
      AND (valid_to_date IS NULL OR valid_to_date >= CURRENT_DATE)
),

-- All items with their BOM status
cte__collector AS (
    SELECT
        i.item_id               AS collector_item_id,
        i.item_name             AS collector_item_name,
        i.item_type             AS collector_item_type,
        i.item_group_id         AS collector_item_group_id,
        i.data_area_id          AS collector_data_area_id,

        -- Has active BOM flag
        CASE
            WHEN bv.item_id IS NOT NULL THEN TRUE
            ELSE FALSE
        END AS has_active_bom

    FROM cte__items i
    LEFT JOIN cte__bom_versions bv
        ON i.item_id = bv.item_id
       AND i.data_area_id = bv.data_area_id
)

SELECT * FROM cte__collector
-- Filter to items with BOMs when used as recursion anchor:
-- WHERE has_active_bom = TRUE
-- AND item_type = 2  -- BOM type items only (optional filter)
```

### 4.3 Null Value Defaults (Alignment with SAP)

| D365 Field | Default | SAP Equivalent |
|------------|---------|----------------|
| `data_area_id` | (required) | werks = 'XX00' |
| `has_active_bom` | FALSE | stlan = 'X' |

---

## 5. Multi-Level Recursive Expansion

### 5.1 Purpose

Explodes BOMs recursively up to 20 levels deep, maintaining the hierarchy path from top-level product to leaf components.

### 5.2 Recursive Algorithm

```
ALGORITHM: BOM Multi-Level Expansion
INPUT: Single-level BOM relationships, Collector products
OUTPUT: Flattened multi-level BOM with hierarchy tracking

1. ANCHOR (Level 1):
   - Select products from Collector (e.g., item_type = 2)
   - Join to Single-Level BOM to get direct components
   - Set iteration=1, nodelevel=1

2. RECURSIVE (Levels 2-20):
   FOR each row in previous iteration WHERE:
     - iteration < 20 (safety limit)
     - is_active_assembly = TRUE (component has active BOM)
     - is_phantom = FALSE (not a phantom assembly)
   DO:
     - Take component_item_id as new parent
     - Join to Single-Level BOM matching:
       - component becomes new parent (component_item_id = parent_item_id)
       - Same legal entity (data_area_id)
     - Increment iteration
     - Increment nodelevel
   END

3. TERMINATION:
   Recursion stops when:
   - iteration >= 20 (max depth reached)
   - is_active_assembly = FALSE (leaf component)
   - is_phantom = TRUE (phantom - skip but continue with children)
```

### 5.3 Complete SQL Implementation

```sql
-- ============================================================
-- MULTI-LEVEL BOM EXPLOSION
-- Analogous to SAP's mart_dim_bill_of_materials__multi_level
-- ============================================================

-- Assumes these views/tables exist:
--   mart_dim_bom__single_level   (from Section 3)
--   mart_dim_bom__collector      (from Section 4)

WITH RECURSIVE cte__bomitems AS (

    -- =========================================================
    -- ANCHOR: Level 1 - Direct components of top-level products
    -- =========================================================
    SELECT
        1                                   AS iteration,
        1                                   AS nodelevel,

        -- Collector (top-level product) info - preserved throughout
        collector.collector_item_id         AS collector_item_id,
        collector.collector_item_name       AS collector_item_name,
        collector.collector_data_area_id    AS collector_data_area_id,

        -- Single-level BOM data
        sglv.parent_item_id,
        sglv.parent_item_name,
        sglv.bom_id,
        sglv.line_num,
        sglv.position,
        sglv.component_item_id,
        sglv.component_item_name,
        sglv.component_item_type,
        sglv.component_item_group_id,
        sglv.bom_qty,
        sglv.bom_qty_serie,
        sglv.unit_id,
        sglv.bom_type,
        sglv.is_assembly,
        sglv.is_active_assembly,
        sglv.is_phantom,
        sglv.data_area_id,

        -- Extended quantity calculation
        sglv.bom_qty / NULLIF(sglv.bom_qty_serie, 0) AS extended_qty,

        -- Hierarchy path for visualization and cycle detection
        CAST(collector.collector_item_id || ' > ' || sglv.component_item_id AS VARCHAR(4000)) AS hierarchy_path

    FROM mart_dim_bom__collector collector

    INNER JOIN mart_dim_bom__single_level sglv
        ON collector.collector_item_id = sglv.parent_item_id
       AND collector.collector_data_area_id = sglv.data_area_id

    WHERE collector.has_active_bom = TRUE
      -- Optional: Filter to specific item types
      -- AND collector.collector_item_type = 2

    UNION ALL

    -- =========================================================
    -- RECURSIVE: Levels 2-20 - Traverse into sub-assemblies
    -- =========================================================
    SELECT
        prev.iteration + 1                  AS iteration,
        prev.nodelevel + 1                  AS nodelevel,

        -- Collector preserved from anchor
        prev.collector_item_id,
        prev.collector_item_name,
        prev.collector_data_area_id,

        -- New parent = previous component
        sglv.parent_item_id,
        sglv.parent_item_name,
        sglv.bom_id,
        sglv.line_num,
        sglv.position,
        sglv.component_item_id,
        sglv.component_item_name,
        sglv.component_item_type,
        sglv.component_item_group_id,
        sglv.bom_qty,
        sglv.bom_qty_serie,
        sglv.unit_id,
        sglv.bom_type,
        sglv.is_assembly,
        sglv.is_active_assembly,
        sglv.is_phantom,
        sglv.data_area_id,

        -- Cumulative extended quantity
        prev.extended_qty * (sglv.bom_qty / NULLIF(sglv.bom_qty_serie, 0)) AS extended_qty,

        -- Append to hierarchy path
        CAST(prev.hierarchy_path || ' > ' || sglv.component_item_id AS VARCHAR(4000)) AS hierarchy_path

    FROM cte__bomitems prev

    -- Join to get this component's children
    INNER JOIN mart_dim_bom__single_level sglv
        ON prev.component_item_id = sglv.parent_item_id
       AND prev.collector_data_area_id = sglv.data_area_id

    WHERE prev.iteration < 20                   -- Safety limit (max depth)
      AND prev.is_active_assembly = TRUE        -- Has active BOM to traverse
      AND prev.is_phantom = FALSE               -- Not a phantom (skip)
      -- Cycle detection: prevent infinite loops
      AND POSITION(sglv.component_item_id IN prev.hierarchy_path) = 0
)

SELECT * FROM cte__bomitems
ORDER BY collector_item_id, hierarchy_path
```

### 5.4 Recursion Control Fields

| Field | Purpose |
|-------|---------|
| `iteration` | Recursion counter (1-20) |
| `nodelevel` | Depth in hierarchy (starts at 1) |
| `collector_item_id` | Top-level product (preserved throughout) |
| `is_active_assembly` | TRUE if component has active BOM to traverse |
| `is_phantom` | TRUE = phantom assembly (skip in recursion) |
| `extended_qty` | Cumulative quantity through hierarchy |
| `hierarchy_path` | Full path string for visualization & cycle detection |

### 5.5 Termination Conditions

1. **Max Depth**: `iteration >= 20` - Prevents infinite loops
2. **Leaf Node**: `is_active_assembly = FALSE` - Component has no BOM
3. **Phantom**: `is_phantom = TRUE` - Skip this level
4. **Cycle Detection**: `POSITION(component IN hierarchy_path) = 0`

---

## 6. Access Control (Serving Layer)

### 6.1 Purpose

Filters data based on user's governance permissions using session context.

### 6.2 SQL Implementation

```sql
WITH cte_source AS (
    SELECT *
    FROM mart_dim_bom__multi_level
),

-- Get user's allowed legal entities from governance table
cte_restricted_companies AS (
    SELECT DISTINCT data_area_id
    FROM governance
    WHERE user_id = SESSION_USER()
),

-- Check if user has all-access
cte_has_full_access AS (
    SELECT is_admin
    FROM governance
    WHERE user_id = SESSION_USER()
),

-- Apply access filter
cte_filtered AS (
    SELECT *
    FROM cte_source
    WHERE data_area_id IN (SELECT data_area_id FROM cte_restricted_companies)
       OR TRUE IN (SELECT is_admin FROM cte_has_full_access)
)

SELECT * FROM cte_filtered
```

---

## 7. Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                  D365 F&O (SOURCE SYSTEM)                       │
├─────────────────────────────────────────────────────────────────┤
│  InventTable  ──────────────────────────────────┐               │
│  BOMTable     ──────────────────────────────┐   │               │
│  BOMVersion   ─────────────────────────┐    │   │               │
│  BOM (Lines)  ────────────────────┐    │    │   │               │
└───────────────────────────────────┼────┼────┼───┼───────────────┘
                                    │    │    │   │
                                    ▼    ▼    ▼   ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ODATA EXTRACTION                             │
├─────────────────────────────────────────────────────────────────┤
│   OAuth2 Authentication → Bearer Token                          │
│   GET /data/BillOfMaterialsLinesV2?cross-company=true           │
│   Pagination: $top=10000&$skip=N                                │
└─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                    01_STAGING LAYER                             │
├─────────────────────────────────────────────────────────────────┤
│   stg_d365__items          (from ReleasedProductsV2)            │
│   stg_d365__bom_versions   (from BillOfMaterialsVersionsV2)     │
│   stg_d365__bom_lines      (from BillOfMaterialsLinesV2)        │
└─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                    02_MARTS LAYER                               │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │     mart_dim_bom__single_level                           │   │
│  │     ──────────────────────────                           │   │
│  │     • Joins BOMVersion→BOM Lines→InventTable             │   │
│  │     • Date validity filters                              │   │
│  │     • Output: Parent→Component single-level relationships│   │
│  │     • Key fields: is_assembly, is_active_assembly        │   │
│  └──────────────────────────────────────────────────────────┘   │
│                             │                                   │
│  ┌──────────────────────────┼───────────────────────────────┐   │
│  │     mart_dim_bom__collector                              │   │
│  │     ──────────────────────                               │   │
│  │     • All items from InventTable                         │   │
│  │     • LEFT JOIN to BOMVersion for has_active_bom flag    │   │
│  │     • Output: Product dimension table                    │   │
│  └──────────────────────────┼───────────────────────────────┘   │
│                             │                                   │
│  ┌──────────────────────────▼───────────────────────────────┐   │
│  │     mart_dim_bom__multi_level                            │   │
│  │     ─────────────────────────                            │   │
│  │     • WITH RECURSIVE cte__bomitems                       │   │
│  │     • ANCHOR: Collector (has_active_bom = TRUE)          │   │
│  │         → Single-level components                        │   │
│  │     • RECURSIVE: WHERE iteration<20                      │   │
│  │         AND is_active_assembly=TRUE                      │   │
│  │         AND is_phantom=FALSE                             │   │
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
│     • Filter by governance.data_area_id                         │
│     • is_admin = TRUE bypasses filter                           │
└─────────────────────────────────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                    04_APPLICATION LAYER                         │
├─────────────────────────────────────────────────────────────────┤
│     powerbi__bill_of_materials                                  │
│     ──────────────────────────                                  │
│     UNION ALL of:                                               │
│       1. Products (from collector, has_active_bom = TRUE)       │
│       2. Components (from multi-level explosion)                │
│       3. Parts (orphan items not in any BOM)                    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 8. Implementation Checklist

### 8.1 Prerequisites

- [ ] Azure AD application registered with client ID/secret
- [ ] Application configured in D365 F&O (System Admin → Azure AD Applications)
- [ ] Data entities enabled and public
- [ ] Cross-company access configured if multi-company environment
- [ ] Local database/warehouse available for staging

### 8.2 DBT Model Architecture

```
sources/
  └── dynamics365/
      └── bom.yml                    # Source definitions

models/
  └── 01_staging/
      ├── stg_d365__bom_versions.sql # Cleaned BOMVersion data
      ├── stg_d365__bom_lines.sql    # Cleaned BOM lines data
      └── stg_d365__items.sql        # Product master

  └── 02_marts/
      ├── mart_dim_bom__single_level.sql    # Pre-joined edge table
      ├── mart_dim_bom__collector.sql       # Product dimension
      └── mart_dim_bom__multi_level.sql     # Recursive explosion

  └── 03_serving/
      └── dim_bill_of_materials.sql         # Access-controlled view

  └── 04_application/
      └── powerbi__bill_of_materials.sql    # Final consumption layer
```

### 8.3 Extraction Steps

1. **Authenticate**: Obtain OAuth2 bearer token from Azure AD
2. **Discover Schema**: Query `$metadata` endpoint for entity fields
3. **Extract to Staging**: Pull OData entities to local staging tables
4. **Build Single-Level**: Create edge table with assembly flags
5. **Build Collector**: Create product dimension
6. **Multi-Level Explosion**: Run recursive CTE
7. **Apply Access Control**: Filter by governance rules
8. **Final Union**: Combine products + components + orphan parts

---

## 9. Key Differences from SAP

| Aspect | SAP | D365 F&O |
|--------|-----|----------|
| **Header Table** | STKO | BOMTable |
| **Version Link** | MAST (plant-specific) | BOMVersion (item-specific) |
| **Line Items** | STPO (via STAS allocation) | BOM (direct BOMId reference) |
| **Item Master** | MARA | InventTable |
| **Date Validity** | STKO.DATUV + LEAD() | BOMVersion.FromDate/ToDate |
| **Phantom Flag** | STPO.REKRS | BOM.BOMType = 1 |
| **Plant Filter** | werks | data_area_id (legal entity) |
| **BOM Application** | stlan (2, E, V) | N/A (single type) |
| **Alternative BOM** | stlal (01, 02, ...) | BOMVersion ordering |

---

## 10. Troubleshooting

### 10.1 Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Empty results | No active BOMVersion | Check `is_active = TRUE` and `is_approved = TRUE` |
| Missing components | Date filters | Verify FromDate/ToDate validity |
| Circular reference | Item references itself | Add hierarchy_path check to prevent loops |
| Truncated results | OData page limit | Implement pagination with $skip/$top |
| Cross-company failure | Missing parameter | Add `?cross-company=true` to URL |

### 10.2 Circular Reference Prevention

Add to recursive CTE WHERE clause:
```sql
AND POSITION(sglv.component_item_id IN prev.hierarchy_path) = 0
```

Or for SQL Server:
```sql
AND CHARINDEX(sglv.component_item_id, prev.hierarchy_path) = 0
```

---

## 11. References

### Microsoft Documentation
- [OData in D365 F&O](https://learn.microsoft.com/en-us/dynamics365/fin-ops-core/dev-itpro/data-entities/odata)
- [Data Entities Overview](https://learn.microsoft.com/en-us/dynamics365/fin-ops-core/dev-itpro/data-entities/data-entities)
- [Service Endpoints Overview](https://learn.microsoft.com/en-us/dynamics365/fin-ops-core/dev-itpro/data-entities/services-home-page)
- [Create a BOM and BOM Version](https://learn.microsoft.com/en-us/dynamicsax-2012/appuser-itpro/create-a-bom-and-bom-version)

### Community Resources
- [Exploding BOM through recursive CTE](https://www.dynamicsuser.net/t/exploding-bom-through-recursive-cte/49322)
- [SQL Recursive CTE BOM Explosion](https://community.dynamics.com/forums/thread/details/?threadid=a203981d-8f25-42dc-b076-bc783c7ac090)
- [BOM Relations Discussion](https://community.dynamics.com/forums/thread/details/?threadid=7517e55d-1709-4e54-b752-b6fce11a3003)

### Tools
- [d365fo.integrations PowerShell Module](https://www.powershellgallery.com/packages/d365fo.integrations)
- [Microsoft Dynamics AX Integration GitHub](https://github.com/Microsoft/Dynamics-AX-Integration)

---

*Document Version: 2.0*
*Last Updated: 2025-12-20*

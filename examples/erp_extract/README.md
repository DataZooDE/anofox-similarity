# From an ERP Extract to Similar Materials

> **A new pump has no history. Which existing pumps are built like it, and whose sales can stand in for its forecast?**

A runnable path from raw SAP and Dynamics 365 tables to the material search, on a small fictional
extract that carries the traps a real one has. The [ERP extract guide](../../docs/guides/12-from-erp-extract.md)
explains each step; this folder is the code and the data.

## The Story

- **Who:** the demand planner of a fictional pump maker (SAP for the German plants, Dynamics 365
  for two legal entities).
- **Trigger:** the stainless pump KP-65 (`000000000000100300`, D365 `PUMP-65`) launches; the
  forecast needs a history it does not have.
- **Question:** which pumps share its parts, and which of them has enough deliveries to borrow?
- **Answer:** KP-50 (Jaccard 0.6, three months of deliveries). Read without STAS, KP-50 would score
  0.8, because its stainless alternative 02 looks like KP-65's casing.

## Data

Everything is in [`data/`](data/), invented, all columns as text (as an SE16 or ODP export gives them).

| File | Rows | What it exercises |
|------|-----:|-------------------|
| `mara.csv` | 13 | Leading-zero `matnr`; `lvorm = X` on an old seal (dropped); a client-200 copy of KP-40 |
| `makt.csv` | 13 | `spras` in SAP form (`E`, `D`); KP-65 has only a German text |
| `mast.csv` | 6 | KP-40 in plants 1000 and 2000 (one BOM); KP-50 with alternatives 01 and 02 |
| `stko.csv` | 5 | Header per alternative, `stlty = M`, `datuv`; client 200 |
| `stpo.csv` | 18 | A document item (`stlty = D`, drawing `DRW-100100`); KP-40's bearing with a 2023 version flagged `lkenz` (deleted); KP-50's stainless casing (node 5) of alternative 02 only |
| `stas.csv` | 19 | The item selection per alternative: node 4 (grey iron) in 01, node 5 (stainless) in 02 |
| `mseg.csv` | 14 | 601 deliveries (`H`), a 602 reversal (`S`), 101 goods receipts (`S`, not demand), 261/262 issue and reversal of a seal, a client-200 row |
| `bomversion.csv` | 6 | D365: PUMP-40 expired 2019 version and current 2024 version; PUMP-50 unapproved draft; two companies |
| `bom.csv` | 26 | D365 lines; paint `250 g` per `BOMQtySerie` 100 |

## Run

```bash
DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/erp_extract/01_sap_to_similarity.sql
DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/erp_extract/02_d365_to_similarity.sql
```

`scripts/run_examples.sh` runs both and checks the numbers in their `-- Expected:` comments.

## What the Scripts Show

**01 (SAP).** `sap_to_bom_items` with `stas_table`, `bom_usage := '1'` and a client filter gives
12 edges. Without STAS it gives 13 (the stainless casing leaks into alternative 01). On
2022-06-30 KP-40 still has its bearings (9 edges, 10 pieces). The MSEG view keeps deliveries and
issues, nets reversals and drops receipts: KP-40 45, KP-50 30, the seal 22.

| material_id | similarity | shared | total |
|-------------|-----------:|-------:|------:|
| 000000000000100200 (KP-50) | 0.6 | 3 | 5 |
| 000000000000100100 (KP-40) | 0.1429 | 1 | 7 |

**02 (D365).** One BOM per company and item: the current PUMP-40 version, PUMP-50's approved BOM
(not the draft). Paint is 2.5 g per pump. Flattened for usmf: 13 edges; both companies together
merge the two PUMP-40 into one parent (17 edges).

| material_id | similarity | shared | total |
|-------------|-----------:|-------:|------:|
| PUMP-50 | 0.5 | 3 | 6 |
| PUMP-40 | 0.125 | 1 | 8 |

## Traps

| Trap | Symptom | Fix |
|------|---------|-----|
| No STAS | Items of every alternative under one BOM number; similarities too high | Export STAS and pass `stas_table` |
| Two clients | A material of two clients comes once per client; the searches mix test and production BOMs | Keep `client = '100'` of `sap_to_materials_with_desc` and of `sap_to_bom_items` |
| Material numbers in two formats | One export has the internal `000000000000100100`, another the ALPHA-converted `100100` (or a spreadsheet dropped the zeros): MAKT, MSEG and STPO no longer join | Read as text (`all_varchar = true`) and pad purely numeric numbers: `CASE WHEN regexp_full_match(matnr, '[0-9]+') THEN lpad(matnr, 18, '0') ELSE matnr END` |
| `language := 'E'` and a text only in German | Empty `description` | Pass the language the texts are in, or join a second language |
| MSEG read whole | Goods receipts (101) count as demand | Filter `bwart`, sign by `shkzg` |
| Two D365 companies flattened together | One parent with the children of both | Filter `company` before `bom_to_items` |

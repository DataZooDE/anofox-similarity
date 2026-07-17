# anofox_similarity Telemetry

`anofox_similarity` collects **anonymous, privacy-preserving usage telemetry** so
we can see which capabilities are used, on which platforms, and where they run —
and prioritise accordingly. It is **on by default** and **trivial to turn off**.

Telemetry is emitted through the shared
[`DataZooDE/posthog-telemetry`](https://github.com/DataZooDE/posthog-telemetry)
library and follows the cross-product **`telemetry_schema: 2`** envelope.
Ingestion is the EU PostHog cloud.

## How to turn it off

The opt-out is enforced at the transport, not just at the call sites.

```bash
export DATAZOO_DISABLE_TELEMETRY=1        -- environment (1|true|yes)
```

```sql
SET anofox_telemetry_enabled = false;   -- DuckDB setting (per session)
```

> [!IMPORTANT]
> Use the **environment variable** to suppress *everything*, including the single
> `extension_loaded` event. That event is emitted while the extension loads — before any SQL
> statement (including `SET anofox_telemetry_enabled = false`) can run — so the SQL setting
> cannot suppress it. The SQL setting disables all telemetry from the moment it is set (i.e. every
> per-function event); the environment variable additionally covers the load event. When
> `DATAZOO_DISABLE_TELEMETRY` is set, nothing leaves the machine at all.

## The guarantee: bounded, enumerated, non-PII

Every property we send is **either** a constant drawn from a small,
code-controlled enumeration **or** a pure number (durations, counts). The library
additionally clamps every outgoing string to 512 bytes as a backstop.

We **never** send: material numbers, BOM contents, component sets, embeddings,
table or column names, SQL text, `WHERE`/`FILTER` clauses, row or result data, or
error messages. The only function-related string we send is the **fixed,
code-literal function name** from the enumeration below — never its arguments.

## What is collected

### Envelope (attached to every event)

`product` (`anofox_similarity`), `product_version`, `product_edition` (`oss`),
`telemetry_schema` (`2`), `duckdb_version`, `os`, `arch`, `platform`, `is_ci`,
`is_container`, a per-process `$session_id`, and — once associated — the
`deployment` group. `distinct_id` is the SHA-256 of a machine id: a **stable,
pseudonymous** identifier, not tied to any personal data.

### Events

| Event | When | Properties (beyond the envelope) |
|---|---|---|
| `extension_loaded` | the `anofox_similarity` extension loads | — |
| `function_executed` | an instrumented DuckDB function runs — **aggregated** per function per session (not per row) | `function_name`, `call_count`, `duration_ms_p50` |

No `feature_used` or `$exception` events are emitted by this repository.

### Instrumented functions (`function_name` enumeration)

The `function_name` sent is always one of these code-literal constants, recorded
at **bind time** (not on a per-row scan path):

`jaccard_similarity`, `find_similar_materials_jaccard`,
`find_similar_materials_wl_kernel`, `cold_start_analogs`, `infer_predecessors`,
`compute_jaccard_embeddings`, `check_statistics_freshness`,
`compute_transactional_embeddings`, `embedding_backend`, `fuse_embeddings`.

## Function-call aggregation

DuckDB function calls are recorded via `RecordFunctionCall(function_name)`, which
aggregates in-process into a single `function_executed` event per function per
session (carrying `call_count` and `duration_ms_p50`). Every capture site sits in
a function's bind / bind-replace callback, never on a per-row `GetChunk` path, so
a million-row scan produces O(1) telemetry rows, not a firehose.

## Enterprise / account analytics

OSS `anofox_similarity` associates only the `deployment` group, keyed on the
pseudonymous `distinct_id`. There is no license key and no `account` group.

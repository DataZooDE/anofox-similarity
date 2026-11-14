# anofox-similarity

DuckDB extension (C++) for product similarity on ERP master data and requirements coverage
matching. Public surface: SQL functions registered in `src/anofox_similarity_extension.cpp`
(modules in `src/modules/`, AAS in `src/modules/aas/`). Builds for DuckDB v1.5.6 and v2.0 from
one tree (`duckdb-compat` shims).

## Build and test

```bash
GEN=ninja make release                          # extension + ./build/release/duckdb (extension linked in on 1.5.6;
                                                # on 2.0 the shell does not link it: duckdb -unsigned, then
                                                # LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension')
DATAZOO_DISABLE_TELEMETRY=1 make test           # whole SQL suite (test/sql/)
./build/release/test/unittest "test/sql/coverage/*"
make check-docs                                 # README + skills SQL runs, API reference current, skills cover every function
python3 scripts/generate_api_reference.py       # regenerate the per-function blocks in docs/api/*.md
scripts/run_examples.sh                         # examples/*/*.sql with their expected numbers
```

## Docs layout

- `README.md`: overview, quick starts, install of the Claude Code plugin
- `docs/API_REFERENCE.md` + `docs/api/NN-*.md`: one page per area; the text between
  `<!-- generated:functions:start/end -->` is generated, the rest is hand-written
- `docs/guides/`: task guides and tutorials; `docs/aas/index.md`: AAS identifier terms
- `examples/<topic>/*.sql`: runnable, numbers checked

## Claude Code plugin

`.claude-plugin/marketplace.json` → `plugins/anofox-similarity/` (manifest in
`.claude-plugin/plugin.json`, six skills in `skills/*/SKILL.md`). Load in-repo with
`claude --plugin-dir ./plugins/anofox-similarity`.

| Skill | Owns `docs/api` pages |
|---|---|
| `anofox-similarity-material-search` | 01, 02, 03 |
| `anofox-similarity-erp-embeddings` | 04, 05, 06 (+ `bom_to_items`, `items_to_materials`, `items_to_bom_header`, `items_to_bom_component`) |
| `anofox-similarity-coverage` | 10, 11, 12, 13 |
| `anofox-similarity-classification-review` | 14 |
| `anofox-similarity-evaluation-scale` | 15, 16 |
| `anofox-similarity-aas` | 17 |

## When you add or change a SQL function

1. Description, parameter names and examples in the C++ registration.
2. Add it to `PAGES` in `scripts/generate_api_reference.py`, write the concept text on its
   `docs/api` page, run the generator.
3. Update the owning skill above (signature, gotchas, a runnable example).
4. `make check-docs` and `make test` must pass.

Bump the plugin `version` in `plugin.json`, each SKILL.md `version` and the `**Extension:**` header
lines to the CalVer of each release (`vYYYY.MM.DD`), and the newest heading of `docs/UPGRADING.md`. `check-docs` checks that plugin.json and the
SKILL.md versions are equal, not that they match the tag.

Result comments in skill SQL can be asserted with `-- => a | b | c` lines (the last statement's
rows); add one when a skill quotes a number a reader will rely on.

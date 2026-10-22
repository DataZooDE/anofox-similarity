# anofox-similarity local compatibility helper

`include/compat_anofox/scalar_bind.hpp` adapts telemetry-only scalar bind
callbacks to the callback signature of each DuckDB line.
`include/compat_anofox/macro_doc.hpp` registers parsed scalar macros with
documentation and the same category defaults as the banner Registrar.
Struct child access now uses
`duckdb_compat::StructChild` from the pinned duckdb-compat submodule, and
function documentation uses the pinned datazoo-banner header directly.

Probe from the workspace root:

```sh
for cc in g++ clang++; do
  for tree in wt156 wt20; do
    "$cc" -std=c++17 -fsyntax-only \
      -Iwt156/compat-anofox/include -Iwt156/duckdb-compat/include \
      -Iwt156/datazoo-banner/include \
      -I"$tree/duckdb/src/include" \
      wt156/compat-anofox/test/probe.cpp
  done
done
```

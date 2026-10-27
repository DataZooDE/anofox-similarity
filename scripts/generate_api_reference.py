#!/usr/bin/env python3
"""Regenerate the per-function reference blocks in docs/api/*.md from the extension's own
function metadata (duckdb_functions(): descriptions, parameters, examples), so the reference
cannot drift from the code.

Run from the repository root after a release build:

    python3 scripts/generate_api_reference.py          # rewrite the generated blocks
    python3 scripts/generate_api_reference.py --check  # exit 1 if a block is out of date

Each page keeps its hand-written guide text; only the part between the markers
<!-- generated:functions:start --> and <!-- generated:functions:end --> is replaced.
"""

import json
import os
import subprocess
import sys

DUCKDB = "./build/release/duckdb"
START = "<!-- generated:functions:start -->"
END = "<!-- generated:functions:end -->"

# page -> functions documented on it, in reading order
PAGES = {
    "01-similarity-scalars.md": [
        "jaccard_similarity", "wl_kernel_similarity", "wl_fingerprint", "tversky_similarity", "containment",
        "weighted_jaccard", "weighted_containment", "weighted_tversky",
    ],
    "02-material-search.md": [
        "find_similar_materials_jaccard", "find_similar_materials_wl_kernel", "infer_predecessors",
        "cold_start_analogs",
    ],
    "03-bom-traversal.md": [
        "bom_explosion_1level", "bom_explosion_multilevel", "bom_where_used", "bom_common_components",
        "create_bom_property_graph", "bom_dfs_neighborhood", "bom_to_items", "items_to_bom",
    ],
    "04-embeddings.md": [
        "compute_textual_embeddings", "embed_text", "embedding_backend", "compute_transactional_embeddings",
        "compute_fused_embeddings", "fuse_embeddings", "compute_jaccard_embeddings", "extract_ts_features",
        "recompute_embedding_statistics", "check_statistics_freshness", "compute_domain_specific_statistics",
    ],
    "05-erp-transformations.md": [
        "sap_to_bom_items", "sap_to_materials", "sap_to_materials_with_desc", "extract_material_descriptions",
        "dynamics365_to_bom_header", "dynamics365_to_bom_component", "dynamics365_to_materials",
    ],
    "06-infrastructure.md": [
        "aggregate_material_components", "filter_recent_movements", "check_anofox_forecast_available",
        "check_duckpgq_available",
    ],
    "10-covering-search.md": ["find_covering_entities", "find_covering_entities_for"],
    "11-quantities.md": ["quantity_units", "parse_quantity", "normalize_quantity", "value_satisfies", "compat_score"],
    "12-property-trees.md": ["tree_from_json", "validate_trees", "tree_anchors", "tree_class_multiset", "tree_multiset"],
    "13-coverage-kernel.md": ["tree_coverage", "tree_coverage_trace", "coverage_recall", "coverage_kernel"],
    "14-classification-and-review.md": [
        "classify_to_catalogue", "maxsim", "bhattacharyya", "calibrated_cosine", "cosine_floor",
        "vocabulary_review_queue", "apply_vocabulary_review", "uncertain_decisions",
    ],
    "15-evaluation.md": [
        "eval_ranking", "eval_calibration", "threshold_at_precision", "bootstrap_ci", "fit_coverage_weights",
    ],
    "16-lsh-and-clustering.md": [
        "weighted_minhash", "minhash_similarity", "lsh_bands", "minhash_index", "minhash_bands",
        "find_covering_entities_lsh", "connected_components", "cluster_matches",
    ],
}

KIND = {
    "scalar": "Scalar function",
    "table": "Table function",
    "macro": "Scalar macro",
    "table_macro": "Table macro",
}


def metadata(names):
    quoted = ", ".join("'" + n + "'" for n in names)
    sql = (
        "SELECT function_name, function_type, return_type, array_to_string(parameters, chr(31)) AS parameters, "
        "array_to_string(parameter_types::VARCHAR[], chr(31)) AS parameter_types, description, "
        "array_to_string(examples, chr(30)) AS examples "
        f"FROM duckdb_functions() WHERE function_name IN ({quoted}) ORDER BY function_name;"
    )
    env = dict(os.environ, DATAZOO_DISABLE_TELEMETRY="1")
    out = subprocess.run([DUCKDB, "-json", "-c", sql], capture_output=True, text=True, env=env, check=True).stdout
    rows = json.loads(out) if out.strip() else []
    by_name = {}
    for r in rows:
        # lists come joined with control characters (the shell's JSON mode does not quote list items)
        r["parameters"] = r["parameters"].split("\x1f") if r.get("parameters") else []
        r["parameter_types"] = r["parameter_types"].split("\x1f") if r.get("parameter_types") else []
        r["examples"] = r["examples"].split("\x1e") if r.get("examples") else []
        by_name.setdefault(r["function_name"], []).append(r)
    return by_name


def signature(row):
    params = row["parameters"] or []
    types = row["parameter_types"] or []
    if row["function_type"] == "scalar" and len(types) == len(params):
        args = ", ".join(p if t in (None, "NULL") else f"{p} {t}" for p, t in zip(params, types))
        ret = f" → {row['return_type']}" if row.get("return_type") else ""
        return f"{row['function_name']}({args}){ret}"
    # table functions report named-parameter types in their own order: names only
    return f"{row['function_name']}({', '.join(params)})"


def render(name, rows):
    rows = sorted(rows, key=lambda r: (len(r["parameters"]), r["parameter_types"]))
    if not rows:
        return f"### `{name}`\n\n*Not registered in this build.*\n"
    first = rows[0]
    kind = KIND.get(first["function_type"], first["function_type"])
    lines = [f"### `{name}`", "", f"*{kind}*", ""]
    description = (first.get("description") or "").strip()
    if description:
        lines += [description, ""]
    signatures = []
    for r in rows:
        s = signature(r)
        if s not in signatures:
            signatures.append(s)
    lines += ["```sql"] + signatures + ["```", ""]
    examples = []
    for r in rows:
        for e in r.get("examples") or []:
            if e and e not in examples:
                examples.append(e)
    if examples:
        lines += ["**Examples**", "", "```sql"] + [e if e.rstrip().endswith(";") or "--" in e else e + ";" for e in examples] + ["```", ""]
    return "\n".join(lines)


def block(page, by_name):
    parts = [START, "", "## Function reference", "",
             "*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*", ""]
    for name in PAGES[page]:
        parts.append(render(name, by_name.get(name, [])))
    parts.append(END)
    return "\n".join(parts)


TITLES = {
    "00-conventions.md": "Conventions: naming, parameters, errors, performance",
    "01-similarity-scalars.md": "Similarity scalars: Jaccard, WL kernel, Tversky, containment, weighted measures",
    "02-material-search.md": "Material search, predecessors and cold start",
    "03-bom-traversal.md": "BOM traversal and property graph",
    "04-embeddings.md": "Embeddings and vector search (textual embeddings are a placeholder)",
    "05-erp-transformations.md": "ERP transformations (SAP, Dynamics 365)",
    "06-infrastructure.md": "Helpers, dependency checks and the tables the extension creates",
    "10-covering-search.md": "Covering search over feature multisets",
    "11-quantities.md": "Quantities: units, parsing, direction-aware comparison",
    "12-property-trees.md": "Property trees: load, validate, summarise",
    "13-coverage-kernel.md": "Coverage kernel: ranking with gaps and an explanation",
    "14-classification-and-review.md": "Classification onto a catalogue and vocabulary review",
    "15-evaluation.md": "Evaluation and tuning",
    "16-lsh-and-clustering.md": "Sub-linear recall (LSH) and entity clustering",
}


def index_block(by_name):
    lines = [START, "", "| Page | Topic | Functions |", "|---|---|---|"]
    for page, title in TITLES.items():
        names = PAGES.get(page, [])
        links = ", ".join(f"[`{n}`](api/{page}#{n.replace('_', '_')})" for n in names) or "—"
        lines.append(f"| [{page[:2]}](api/{page}) | {title} | {links} |")
    total = sum(len(v) for v in PAGES.values())
    lines += ["", f"{total} functions in total.", "", END]
    return "\n".join(lines)


def main():
    check = "--check" in sys.argv
    all_names = [n for names in PAGES.values() for n in names]
    by_name = metadata(all_names)
    stale = []
    for page in PAGES:
        path = os.path.join("docs", "api", page)
        text = open(path).read()
        new_block = block(page, by_name)
        if START in text:
            head, rest = text.split(START, 1)
            tail = rest.split(END, 1)[1] if END in rest else ""
            new_text = head + new_block + tail
        else:
            new_text = text.rstrip("\n") + "\n\n" + new_block + "\n"
        if new_text != text:
            stale.append(page)
            if not check:
                open(path, "w").write(new_text)
    index_path = os.path.join("docs", "API_REFERENCE.md")
    index_text = open(index_path).read()
    if START in index_text:
        head, rest = index_text.split(START, 1)
        new_index = head + index_block(by_name) + rest.split(END, 1)[1]
        if new_index != index_text:
            stale.append("API_REFERENCE.md")
            if not check:
                open(index_path, "w").write(new_index)
    if check and stale:
        print("out of date: " + ", ".join(stale))
        sys.exit(1)
    print(("checked " if check else "updated ") + f"{len(PAGES)} pages" + (f", rewrote {len(stale)}" if stale and not check else ""))


if __name__ == "__main__":
    main()

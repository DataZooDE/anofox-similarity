"""Tests for the coverage-matching test dataset (automotive seating systems).

The generator is the specification of the benchmark, so these tests pin the
properties HANDOVER.md §3 asks for: determinism, size, leakage, noise rates,
vector calibration, oracle semantics and domain neutrality.
"""

import csv
import gzip
import hashlib
import itertools
import json
import os
import random
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pytest

from scripts.coverage import generate
from scripts.coverage import reference as ref
from scripts.coverage.domain import ORDINALS

SEED = 7
REPO = Path(__file__).resolve().parents[2]
COMMITTED = REPO / "test" / "data" / "coverage"


def _run(out: Path) -> None:
    generate.main(["--seed", str(SEED), "--output", str(out)])


@pytest.fixture(scope="session")
def data_dir(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("coverage")
    _run(out)
    return out


_CACHE: dict = {}


def read(data_dir: Path, name: str) -> list[dict]:
    key = (data_dir, name)
    if key not in _CACHE:
        with gzip.open(data_dir / f"{name}.csv.gz", "rt", encoding="utf-8", newline="") as fh:
            _CACHE[key] = list(csv.DictReader(fh))
    return _CACHE[key]


def vec(s: str) -> np.ndarray:
    return np.asarray(json.loads(s), dtype=np.float64)


def digest(root: Path, exclude=()) -> dict[str, str]:
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file() and not any(str(p.relative_to(root)).startswith(e) for e in exclude)
    }


def gold_class(data_dir) -> dict:
    return {(g["tree_id"], g["node_id"]): g["class_id"] for g in read(data_dir, "gold_item_class")}


EXPECTED_FILES = [
    "catalogue_classes", "catalogue_attributes", "compatibility_rules",
    "internal_vocabulary", "module_nodes", "module_attributes",
    "programme_nodes", "programme_attributes", "request_nodes", "request_attributes",
    "programme_class_multiset", "programme_class_multiset_unreviewed",
    "phrasings", "unseen_requirements",
    "requests", "catalogue_vectors", "request_item_vectors", "vocabulary_vectors",
    "unseen_vectors", "attribute_name_vectors", "customers",
    "gold_item_class", "gold_vocabulary", "gold_request_attributes", "gold_unseen",
    "gold_requests", "gold_coverage", "gold_best_applications", "gold_customer_clusters",
]


# ---------------------------------------------------------------- files, determinism, size

def test_all_files_exist(data_dir):
    for name in EXPECTED_FILES:
        assert (data_dir / f"{name}.csv.gz").is_file(), name
    assert len(list((data_dir / "requests_json").glob("*.json"))) == len(read(data_dir, "gold_requests"))


def test_deterministic_across_processes(data_dir, tmp_path):
    """Two CLI runs with different hash seeds produce byte-identical files."""
    digests = []
    for hash_seed in ("1", "2"):
        out = tmp_path / f"run{hash_seed}"
        env = dict(os.environ, PYTHONHASHSEED=hash_seed)
        subprocess.run([sys.executable, "-m", "scripts.coverage.generate", "--seed", str(SEED),
                        "--output", str(out)], check=True, env=env)
        digests.append(digest(out))
    assert digests[0] == digests[1] == digest(data_dir)
    for p in data_dir.glob("*.csv.gz"):
        assert p.read_bytes()[4:8] == b"\x00\x00\x00\x00", f"gzip mtime set in {p.name}"


def content_digest(root: Path, exclude=()) -> dict[str, str]:
    """Hash decompressed content: gzip bytes depend on the zlib build."""
    out = {}
    for p in sorted(root.rglob("*")):
        rel = str(p.relative_to(root))
        if p.is_file() and not any(rel.startswith(e) for e in exclude):
            raw = p.read_bytes()
            out[rel] = hashlib.sha256(gzip.decompress(raw) if p.suffix == ".gz" else raw).hexdigest()
    return out


def test_committed_data_is_current(data_dir):
    if not COMMITTED.exists():
        pytest.skip("dataset not generated into the repository yet")
    assert content_digest(COMMITTED, exclude=("mini", "README.md")) == content_digest(data_dir)
    assert (COMMITTED / "README.md").is_file()
    assert (COMMITTED / "mini").is_dir() and any((COMMITTED / "mini").iterdir())


def test_total_size_within_budget(data_dir):
    total = sum(p.stat().st_size for p in data_dir.rglob("*") if p.is_file())
    assert total <= 8 * 1024 * 1024, f"{total / 1e6:.2f} MB"


# ---------------------------------------------------------------- catalogue

def test_catalogue_shape(data_dir):
    classes = read(data_dir, "catalogue_classes")
    attrs = read(data_dir, "catalogue_attributes")
    assert 20 <= len(classes) <= 24
    per_class = Counter(a["class_id"] for a in attrs)
    assert all(2 <= per_class[c["class_id"]] <= 4 for c in classes)
    bounds = Counter(a["bound"] for a in attrs)
    assert bounds["at_least"] > 0 and bounds["at_most"] > 0 and bounds["within"] > 0
    assert bounds["equals"] > 0
    # at least one class carries two attributes of the same dimension, so the
    # attribute name has to discriminate (BR-16); otherwise names never matter
    dims = Counter((a["class_id"], a["dimension"]) for a in attrs)
    assert max(dims.values()) == 2


def test_ordinal_attributes_have_compatibility_rules(data_dir):
    attrs = read(data_dir, "catalogue_attributes")
    rules = read(data_dir, "compatibility_rules")
    ordinal_dims = {a["dimension"] for a in attrs if a["dimension"] in ORDINALS}
    assert ordinal_dims == set(ORDINALS)
    for dim, scale in ORDINALS.items():
        pairs = {(r["required"], r["provided"]): float(r["score"]) for r in rules if r["dimension"] == dim}
        assert len(pairs) == len(scale) ** 2
        # a better class satisfies, a worse one does not
        assert pairs[(scale[1], scale[2])] == 1.0
        assert pairs[(scale[2], scale[1])] < 1.0


UNIT_PATTERNS = {
    "mm": r"\d\s*mm\b", "cm": r"\d\s*cm\b", "kN": r"\d\s*kN\b", "N": r"\d\s*N\b(?!·)",
    "W": r"\d\s*W\b", "s": r"\d\s*s\b", "min": r"\d\s*min\b(?!\.)", "ms": r"\d\s*ms\b",
    "dB(A)": r"\d\s*dB\(A\)", "kg": r"\d\s*kg\b", "g": r"\d\s*g\b", "A": r"\d\s*A\b",
    "mA": r"\d\s*mA\b", "°C": r"\d\s*°C", "EUR": r"\d\s*(EUR|€)", "cycles": r"\d\s*(cycles|Zyklen)\b",
    "IP": r"\bIP\d\d\b",
}


def test_attribute_names_carry_no_bound_words(data_dir):
    for a in read(data_dir, "request_attributes"):
        assert not BOUND_WORDS.search(a["name"]), a["name"]


def test_units_used_in_requests(data_dir):
    texts = "\n".join(a["value_text"] for a in read(data_dir, "request_attributes"))
    for unit, pat in UNIT_PATTERNS.items():
        assert re.search(pat, texts), unit


# ---------------------------------------------------------------- module library

def _roots(nodes):
    return [n for n in nodes if n["parent_id"] == ""]


def test_module_library_or_tree(data_dir):
    nodes = read(data_dir, "module_nodes")
    roots = _roots(nodes)
    kind = {(n["tree_id"], n["node_id"]): n["node_kind"] for n in nodes}
    configs = [n for n in nodes if n["parent_id"] and kind[(n["tree_id"], n["parent_id"])] == "variant_group"
               and n["node_kind"] == "variant_group"]
    apps = [n for n in nodes if n["application_id"]]
    assert 35 <= len(roots) <= 45
    assert 80 <= len(configs) <= 100
    assert 140 <= len(apps) <= 160
    for n in nodes:
        if n["parent_id"] and kind[(n["tree_id"], n["parent_id"])] == "variant_group":
            assert n["edge_type"] == "OR", n
    # every application is either an item or a container of AND items
    children = defaultdict(list)
    for n in nodes:
        children[(n["tree_id"], n["parent_id"])].append(n)
    for a in apps:
        if a["node_kind"] == "container":
            kids = children[(a["tree_id"], a["node_id"])]
            assert len(kids) >= 2 and all(k["node_kind"] == "item" and k["edge_type"] == "AND" for k in kids)
        else:
            assert a["node_kind"] == "item"


def _app_items(nodes, gold):
    """application_id -> list of (class_id, count, item node)."""
    by_key = {(n["tree_id"], n["node_id"]): n for n in nodes}
    out = defaultdict(list)
    for n in nodes:
        if n["node_kind"] != "item":
            continue
        app = n if n["application_id"] else by_key[(n["tree_id"], n["parent_id"])]
        out[app["application_id"]].append((gold[(n["tree_id"], n["node_id"])], float(n["count"]), n))
    return out


def test_combo_modules_cover_several_classes(data_dir):
    nodes = read(data_dir, "module_nodes")
    items = _app_items(nodes, gold_class(data_dir))
    tree_of = {n["application_id"]: n["tree_id"] for n in nodes if n["application_id"]}
    combo_trees = {tree_of[a] for a, its in items.items() if len({c for c, _, _ in its}) >= 2}
    assert 4 <= len(combo_trees) <= 5


# ---------------------------------------------------------------- programmes

def test_programmes(data_dir):
    nodes = read(data_dir, "programme_nodes")
    progs = {n["tree_id"] for n in nodes}
    assert 280 <= len(progs) <= 320
    items = [n for n in nodes if n["node_kind"] == "item"]
    with_module = {n["tree_id"] for n in items if n["module_ref"]}
    capability_only = progs - with_module
    assert 0.35 <= len(capability_only) / len(progs) <= 0.45
    assert {n["source"] for n in nodes} == {"internal"}


def test_provided_ranges_tell_at_most_from_at_least(data_dir):
    """Some provided values on at_least/at_most attributes are ranges, so a p_lo/p_hi
    swap in value_satisfies changes results on real requests."""
    tmpl = {(a["class_id"], a["attr_key"]): a["bound"] for a in read(data_dir, "catalogue_attributes")}
    gold = gold_class(data_dir)
    directional = [a for a in read(data_dir, "programme_attributes")
                   if tmpl[(gold[(a["tree_id"], a["node_id"])], a["attr_id"])] in ("at_least", "at_most")
                   and a["category"] == ""]
    ranged = [a for a in directional if float(a["lo"]) < float(a["hi"])]
    assert 0.08 <= len(ranged) / len(directional) <= 0.20
    # pairs (requirement, source item) where the swapped rule gives another answer
    swapped = 0
    for g in read(data_dir, "gold_request_attributes"):
        if g["bound"] in ("at_least", "at_most") and g["category"] == "":
            v, lo, hi = float(g["lo"]), float(g["src_lo"]), float(g["src_hi"])
            right = ref.oracle_satisfies(g["bound"], v, v, lo, hi)
            wrong = (lo >= v) if g["bound"] == "at_least" else (hi <= v)
            swapped += right != wrong
    assert swapped >= 50, swapped


def test_programme_module_refs_copy_module_items(data_dir):
    """A programme item that references a module item has its class, count and
    provided values; combo modules contribute one item per class, so counts add
    up across classes."""
    gold = gold_class(data_dir)
    mod_nodes = {n["node_id"]: n for n in read(data_dir, "module_nodes")}
    mod_attrs = defaultdict(dict)
    for a in read(data_dir, "module_attributes"):
        mod_attrs[a["node_id"]][a["attr_id"]] = (a["lo"], a["hi"], a["category"])
    prog_attrs = defaultdict(dict)
    for a in read(data_dir, "programme_attributes"):
        prog_attrs[(a["tree_id"], a["node_id"])][a["attr_id"]] = (a["lo"], a["hi"], a["category"])
    combo_refs = 0
    for n in read(data_dir, "programme_nodes"):
        if n["node_kind"] != "item" or not n["module_ref"]:
            continue
        m = mod_nodes[n["module_ref"]]
        assert m["node_kind"] == "item"
        assert gold[(n["tree_id"], n["node_id"])] == gold[(m["tree_id"], m["node_id"])]
        assert float(n["count"]) == float(m["count"])
        assert prog_attrs[(n["tree_id"], n["node_id"])] == mod_attrs[m["node_id"]]
        combo_refs += not m["application_id"]
    assert combo_refs > 0


def test_misleading_wordings_touch_most_programmes(data_dir):
    vocab = read(data_dir, "gold_vocabulary")
    assert {(v["wording"], v["source"]) for v in vocab} == \
        {(v["wording"], v["source"]) for v in read(data_dir, "internal_vocabulary")}
    misleading = {(v["wording"], v["source"]) for v in vocab if v["is_misleading"] == "true"}
    assert 3 <= len(misleading) <= 4
    assert 45 <= len(vocab) <= 55
    for v in vocab:
        assert (v["apparent_class_id"] != v["class_id"]) == (v["is_misleading"] == "true")
    nodes = read(data_dir, "programme_nodes")
    progs = {n["tree_id"] for n in nodes}
    touched = {n["tree_id"] for n in nodes if (n["label"], n["source"]) in misleading}
    assert len(touched) / len(progs) > 0.60


def _multiset(nodes, label_class):
    out = Counter()
    for n in nodes:
        if n["node_kind"] == "item":
            out[(n["tree_id"], label_class(n))] += float(n["count"])
    return dict(out)


def _read_multiset(data_dir, name):
    return {(r["entity_id"], r["feature_id"]): float(r["weight"]) for r in read(data_dir, name)}


def test_programme_class_multisets(data_dir):
    gold = gold_class(data_dir)
    nodes = read(data_dir, "programme_nodes")
    reviewed = _multiset(nodes, lambda n: gold[(n["tree_id"], n["node_id"])])
    assert _read_multiset(data_dir, "programme_class_multiset") == reviewed
    apparent = {(v["wording"], v["source"]): v["apparent_class_id"] for v in read(data_dir, "gold_vocabulary")}
    unreviewed = _multiset(nodes, lambda n: apparent[(n["label"], n["source"])])
    assert _read_multiset(data_dir, "programme_class_multiset_unreviewed") == unreviewed
    assert reviewed != unreviewed


# ---------------------------------------------------------------- requests

def test_request_volume_and_hierarchy(data_dir):
    reqs = read(data_dir, "gold_requests")
    assert 75 <= len(reqs) <= 85
    progs = {n["tree_id"] for n in read(data_dir, "programme_nodes")}
    assert all(r["source_programme_id"] in progs for r in reqs)
    nodes = read(data_dir, "request_nodes")
    items = [n for n in nodes if n["node_kind"] == "item"]
    assert 1350 <= len(items) <= 1650
    public = {r["request_id"]: r for r in read(data_dir, "requests")}
    assert set(public) == {r["request_id"] for r in reqs}
    assert all(public[r["request_id"]]["lang"] == r["lang"] for r in reqs)
    langs = Counter(r["lang"] for r in reqs)
    assert 0.4 <= langs["de"] / len(reqs) <= 0.6
    gran = Counter(r["granularity"] for r in reqs)
    assert gran["coarse"] > 0 and gran["detailed"] > 0
    parent = {(n["tree_id"], n["node_id"]): n["parent_id"] for n in nodes}

    def depth(t, nid):
        d = 0
        while parent[(t, nid)]:
            nid = parent[(t, nid)]
            d += 1
        return d

    depths = Counter(depth(n["tree_id"], n["node_id"]) for n in items)
    assert len(depths) >= 3
    assert any(n["node_kind"] == "container" and n["label"].lower().startswith(("note", "hinweis"))
               for n in nodes)


# An independent bound-word list (not the generator's), EN and DE.
BOUND_WORDS = re.compile(
    r"(at least|at most|min\.|max\.|minimum|maximum|up to|not more than|not less than|or better|±|nominal|Nennwert|"
    r"mindestens|höchstens|bis zu|maximal|minimal|oder besser|≥|≤|>=|<=|"
    r"\d\s*(–|-|…|to|bis)\s*\d|between|zwischen)", re.I)


def test_stretched_and_unqualified_rates(data_dir):
    gold = read(data_dir, "gold_request_attributes")
    raw = {(a["tree_id"], a["node_id"], a["attr_id"]): a for a in read(data_dir, "request_attributes")}
    items = [n for n in read(data_dir, "request_nodes") if n["node_kind"] == "item"]
    stretched_items = {(g["tree_id"], g["node_id"]) for g in gold if g["stretched"] == "true"}
    assert 0.09 <= len(stretched_items) / len(items) <= 0.15
    numeric = [g for g in gold if g["category"] == ""]
    unqualified = [g for g in numeric
                   if not BOUND_WORDS.search(raw[(g["tree_id"], g["node_id"], g["attr_id"])]["value_text"])]
    assert 0.25 <= len(unqualified) / len(numeric) <= 0.35
    # some requirement sentences state several values
    numbers = re.compile(r"\d+(?:[.,]\d+)?\s*(?:mm|cm|kN|N|W|s|ms|min|dB\(A\)|kg|g|A|mA|°C|°|EUR|€|m³/h|l/min)")
    multi = [n for n in items if len(numbers.findall(n["label"])) >= 2]
    assert len(multi) > 100


def test_stretched_attributes_fail_on_their_source(data_dir):
    for g in read(data_dir, "gold_request_attributes"):
        if g["category"]:
            ok = ref.ordinal_satisfies(g["dimension"], g["category"], g["src_category"])
        else:
            ok = ref.oracle_satisfies(g["bound"], float(g["lo"]), float(g["hi"]),
                                      float(g["src_lo"]), float(g["src_hi"]))
        assert ok == (g["stretched"] == "false"), g


def test_unmeetable_requests(data_dir):
    """Unmeetable means no archived programme reaches the BRD flag threshold, with a
    clear margin on both sides so the flag KPI measures the kernel, not rounding."""
    reqs = read(data_dir, "gold_requests")
    unmeetable = [r for r in reqs if r["is_unmeetable"] == "true"]
    assert 5 <= len(unmeetable) <= 20
    for r in reqs:
        best = float(r["best_coverage"])
        assert (best < 0.95) == (r["is_unmeetable"] == "true")
        if r["is_unmeetable"] == "true":
            assert 0.5 <= best <= 0.90, r
        else:
            assert best >= 0.97, r


def test_rankings_are_not_degenerate(data_dir):
    """No request ties every programme; the best is a small group, not the archive."""
    best = defaultdict(float)
    cov = defaultdict(list)
    for c in read(data_dir, "gold_coverage"):
        cov[c["query_id"]].append(float(c["true_coverage"]))
    for q, vals in cov.items():
        top = max(vals)
        n_best = sum(1 for v in vals if v >= top - 1e-9)
        assert n_best < len(vals) * 0.1, (q, n_best)


def test_request_json_matches_tables(data_dir):
    nodes = read(data_dir, "request_nodes")
    attrs = read(data_dir, "request_attributes")
    node_rows = {(n["tree_id"], n["node_id"]): n for n in nodes}
    attr_rows = {(a["tree_id"], a["node_id"], a["attr_id"]): a for a in attrs}
    seen_nodes, seen_attrs = 0, 0
    for path in sorted((data_dir / "requests_json").glob("*.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        tid = doc["tree_id"]

        def walk(n, parent):
            nonlocal seen_nodes, seen_attrs
            row = node_rows[(tid, n["node_id"])]
            assert row["parent_id"] == parent and row["label"] == n["label"] and row["node_kind"] == n["kind"]
            seen_nodes += 1
            for a in n.get("attributes", []):
                assert attr_rows[(tid, n["node_id"], a["attr_id"])]["value_text"] == a["text"]
                seen_attrs += 1
            for c in n.get("children", []):
                walk(c, n["node_id"])

        walk(doc["root"], "")
    assert seen_nodes == len(nodes) and seen_attrs == len(attrs)


# ---------------------------------------------------------------- phrasings / leakage

def test_phrasing_pool_split(data_dir):
    ph = read(data_dir, "phrasings")
    per_class = Counter((p["class_id"], p["split"]) for p in ph)
    for c in {p["class_id"] for p in ph}:
        assert per_class[(c, "dev")] == 9 and per_class[(c, "test")] == 9
    dev = {p["text"].lower() for p in ph if p["split"] == "dev"}
    test = {p["text"].lower() for p in ph if p["split"] == "test"}
    assert not dev & test


def test_test_phrasings_never_in_development_data(data_dir):
    ph = {p["phrasing_id"]: p for p in read(data_dir, "phrasings")}
    # request items use development phrasings of their own class
    for g in read(data_dir, "gold_item_class"):
        if g["phrasing_id"]:
            p = ph[g["phrasing_id"]]
            assert p["split"] == "dev" and p["class_id"] == g["class_id"]
    # catalogue exemplars come only from development phrasings
    for c in read(data_dir, "catalogue_vectors"):
        for pid in json.loads(c["exemplar_phrasing_ids"]):
            assert ph[pid]["split"] == "dev" and ph[pid]["class_id"] == c["class_id"]
    # unseen items use a test phrasing of their own class, and its text appears verbatim
    unseen = {u["item_id"]: u for u in read(data_dir, "unseen_requirements")}
    assert 180 <= len(unseen) <= 220
    for g in read(data_dir, "gold_unseen"):
        p = ph[g["phrasing_id"]]
        assert p["split"] == "test" and p["class_id"] == g["class_id"]
        assert p["text"] in unseen[g["item_id"]]["text"]
    # and no test phrasing text appears in any development-side label
    test_texts = [p["text"] for p in ph.values() if p["split"] == "test"]
    labels = [n["label"] for name in ("request_nodes", "programme_nodes", "module_nodes")
              for n in read(data_dir, name)]
    for t in test_texts:
        assert not any(t in lab for lab in labels), t


# ---------------------------------------------------------------- vectors

def test_vectors_are_normalised_64d(data_dir):
    for name in ["catalogue_vectors", "request_item_vectors", "vocabulary_vectors",
                 "unseen_vectors", "attribute_name_vectors"]:
        for row in read(data_dir, name):
            v = vec(row["vec"])
            assert v.shape == (64,)
            assert abs(np.linalg.norm(v) - 1) < 2e-3
    for name in ["request_item_vectors", "unseen_vectors", "vocabulary_vectors"]:
        for row in read(data_dir, name):
            toks = np.asarray(json.loads(row["tokens"]))
            assert 3 <= len(toks) <= 12 and toks.shape[1] == 64
            assert np.allclose(np.linalg.norm(toks, axis=1), 1, atol=3e-3)


def _classification_inputs(data_dir, items_name, key_cols, gold_lookup):
    cat = read(data_dir, "catalogue_vectors")
    class_ids = [c["class_id"] for c in cat]
    C = np.stack([vec(c["vec"]) for c in cat])
    E = [np.asarray(json.loads(c["exemplar_tokens"])) for c in cat]
    rows = read(data_dir, items_name)
    X = np.stack([vec(r["vec"]) for r in rows])
    T = [np.asarray(json.loads(r["tokens"])) for r in rows]
    src = [r["source"] for r in rows]
    y = [gold_lookup[tuple(r[k] for k in key_cols)] for r in rows]
    return class_ids, C, E, X, T, src, y


def test_centring_lifts_pivot_accuracy_on_dev_phrasings(data_dir):
    class_ids, C, _, X, _, src, y = _classification_inputs(
        data_dir, "request_item_vectors", ["tree_id", "node_id"], gold_class(data_dir))
    raw = ref.top1_accuracy(ref.centroid_scores(X, C), class_ids, y)
    # centring then z-score across the catalogue, as the HLD pipeline does
    centred = ref.top1_accuracy(ref.zscore(ref.centroid_scores(ref.centre_by_source(X, src), C)), class_ids, y)
    assert 0.50 <= raw <= 0.70, raw
    assert 0.80 <= centred <= 0.95, centred
    assert centred - raw >= 0.10


def test_maxsim_beats_centroid_on_held_out_phrasings(data_dir):
    gold = {(g["item_id"],): g["class_id"] for g in read(data_dir, "gold_unseen")}
    class_ids, C, E, X, T, src, y = _classification_inputs(data_dir, "unseen_vectors", ["item_id"], gold)
    centroid = ref.top1_accuracy(ref.zscore(ref.centroid_scores(ref.centre_by_source(X, src), C)), class_ids, y)
    late = ref.top1_accuracy(ref.maxsim_scores(T, E), class_ids, y)
    assert late >= centroid + 0.08, (centroid, late)
    assert 0.85 <= late <= 0.95, late


def test_concept_vectors_use_development_phrasings_only():
    """Concept tokens that occur only in held-out phrasings must not be placed with
    the help of their held-out class labels."""
    from scripts.coverage import vectors as V
    from scripts.coverage.domain import PHRASINGS
    dev = {c for cid in PHRASINGS for ph in PHRASINGS[cid]["dev"] for c in ph.concepts}
    test_only = {c for cid in PHRASINGS for ph in PHRASINGS[cid]["test"] for c in ph.concepts} - dev
    assert test_only
    sp = V.Space(SEED)
    for c in test_only:
        assert c not in sp.concept


def test_misleading_wordings_sit_near_wrong_class(data_dir):
    gv = {(v["wording"], v["source"]): v for v in read(data_dir, "gold_vocabulary")}
    cat = read(data_dir, "catalogue_vectors")
    class_ids = [c["class_id"] for c in cat]
    C = np.stack([vec(c["vec"]) for c in cat])
    rows = read(data_dir, "vocabulary_vectors")
    X = ref.centre_by_source(np.stack([vec(r["vec"]) for r in rows]), [r["source"] for r in rows])
    for row, x in zip(rows, X):
        g = gv[(row["wording"], row["source"])]
        best = class_ids[int(np.argmax(C @ x))]
        assert best == g["apparent_class_id"], row["wording"]


def test_attribute_name_cosine_floor(data_dir):
    rows = read(data_dir, "attribute_name_vectors")
    V = np.stack([vec(r["vec"]) for r in rows])
    keys = [r["attr_key"] for r in rows]
    sims = V @ V.T
    same = np.array([[a == b for b in keys] for a in keys]) & ~np.eye(len(V), dtype=bool)
    unrelated = np.array([[a != b for b in keys] for a in keys])
    assert 0.80 <= float(np.median(sims[unrelated])) <= 0.90
    # synonyms of the same attribute stand clearly above the unrelated floor
    assert float(np.mean(sims[same])) > float(np.percentile(sims[unrelated], 95))


# ---------------------------------------------------------------- oracle

def test_temperature_uses_an_absolute_tolerance():
    """HLD §11 amendment: max. 37 °C against 45 °C is far off, not 2.6 % off."""
    k = 273.15
    assert ref.value_satisfies("at_most", 37 + k, 37 + k, 45 + k, 45 + k, "linear", affine=True) == (0.0, 8.0)
    score, dist = ref.value_satisfies("at_most", 37 + k, 37 + k, 37.5 + k, 37.5 + k, "linear", affine=True)
    assert score == 1.0 and abs(dist - 0.5) < 1e-9


def test_oracle_direction_aware_rules():
    # at_most is satisfied by the low end of the provided range (p_lo <= v)
    assert ref.oracle_satisfies("at_most", 38, 38, 35, 41)
    assert not ref.oracle_satisfies("at_most", 38, 38, 39, 45)
    assert ref.oracle_satisfies("at_most", 38, 38, 38, 38)  # exactly at the limit
    # at_least is satisfied by the high end (p_hi >= v)
    assert ref.oracle_satisfies("at_least", 240, 240, 200, 250)
    assert not ref.oracle_satisfies("at_least", 240, 240, 200, 239)
    assert ref.oracle_satisfies("at_least", 240, 240, 240, 240)
    # within: the provided range must cover the required range
    assert ref.oracle_satisfies("within", 15, 25, 10, 30)
    assert not ref.oracle_satisfies("within", 15, 25, 20, 30)  # partial overlap
    assert not ref.oracle_satisfies("within", 15, 25, 26, 30)  # disjoint
    # equals: relative tolerance 2 %
    assert ref.oracle_satisfies("equals", 200, 200, 203.8, 203.8)
    assert not ref.oracle_satisfies("equals", 200, 200, 204.2, 204.2)
    # ordinal: provided class at least as good as required
    assert ref.ordinal_satisfies("protection_class", "IP54", "IP65")
    assert not ref.ordinal_satisfies("protection_class", "IP65", "IP54")


def test_oracle_assigns_each_offer_item_once():
    """HANDOVER §7.3: one offer item serves one request block. Two driver/passenger
    requirements of the same class cannot both be served by a single offer item."""
    req = [("q1", "seat_track", 1.0, []), ("q2", "seat_track", 1.0, [])]
    one = [("c1", "seat_track", 1.0, {})]
    two = [("c1", "seat_track", 1.0, {}), ("c2", "seat_track", 1.0, {})]
    assert ref.oracle_assignment_coverage(req, one, lambda x, y: 1.0) == 0.5
    assert ref.oracle_assignment_coverage(req, two, lambda x, y: 1.0) == 1.0
    # counts: a 4-zone requirement served by one 2-zone item is half covered
    req4 = [("q1", "seat_heating", 4.0, [])]
    assert ref.oracle_assignment_coverage(req4, [("c1", "seat_heating", 2.0, {})], lambda x, y: 1.0) == 0.5


def _independent_coverage(data_dir):
    """Recompute oracle coverage from the raw tables, without generator code."""
    gold = gold_class(data_dir)
    req_items = defaultdict(list)
    for n in read(data_dir, "request_nodes"):
        if n["node_kind"] == "item":
            req_items[n["tree_id"]].append((n["node_id"], gold[(n["tree_id"], n["node_id"])], float(n["count"])))
    req_attrs = defaultdict(list)
    for g in read(data_dir, "gold_request_attributes"):
        req_attrs[(g["tree_id"], g["node_id"])].append(g)
    prog_items = defaultdict(lambda: defaultdict(list))
    for n in read(data_dir, "programme_nodes"):
        if n["node_kind"] == "item":
            prog_items[n["tree_id"]][gold[(n["tree_id"], n["node_id"])]].append((n["node_id"], float(n["count"])))
    prog_attrs = defaultdict(dict)
    for a in read(data_dir, "programme_attributes"):
        prog_attrs[(a["tree_id"], a["node_id"])][a["attr_id"]] = a

    def sat(x, y):
        if y is None:
            return 0.0
        if x["category"]:
            return float(ref.ordinal_satisfies(x["dimension"], x["category"], y["category"]))
        return float(ref.oracle_satisfies(x["bound"], float(x["lo"]), float(x["hi"]), float(y["lo"]), float(y["hi"])))

    def item_score(nid, rid, n_q, block, pid):
        if not block:
            return 0.0
        xs = req_attrs[(rid, nid)]
        a = max((sum(sat(x, prog_attrs[(pid, c)].get(x["attr_key"])) for x in xs) / len(xs)) if xs else 1.0
                for c, _ in block)
        return a * min(1.0, sum(n for _, n in block) / n_q)

    def coverage(rid, pid):
        # brute force: every offer item of a class goes to exactly one request item of that class
        total = 0.0
        by_cls = defaultdict(list)
        for nid, cls, n_q in req_items[rid]:
            by_cls[cls].append((nid, n_q))
        for cls, qs in by_cls.items():
            cands = prog_items[pid].get(cls, [])
            best = 0.0
            for choice in itertools.product(range(len(qs)), repeat=len(cands)):
                blocks = defaultdict(list)
                for cand, qi in zip(cands, choice):
                    blocks[qi].append(cand)
                best = max(best, sum(item_score(nid, rid, n_q, blocks[i], pid) for i, (nid, n_q) in enumerate(qs)))
            total += best
        return total / len(req_items[rid])

    return coverage


def test_gold_coverage_complete_and_recomputed(data_dir):
    reqs = read(data_dir, "gold_requests")
    progs = sorted({n["tree_id"] for n in read(data_dir, "programme_nodes")})
    cov = read(data_dir, "gold_coverage")
    pairs = {(c["query_id"], c["candidate_id"]): float(c["true_coverage"]) for c in cov}
    assert len(pairs) == len(cov) == len(reqs) * len(progs)
    assert set(pairs) == {(r["request_id"], p) for r in reqs for p in progs}
    best = defaultdict(float)
    for (q, _), v in pairs.items():
        assert 0.0 <= v <= 1.0
        best[q] = max(best[q], v)
    for r in reqs:
        assert abs(best[r["request_id"]] - float(r["best_coverage"])) < 1e-9
        assert abs(pairs[(r["request_id"], r["best_programme_id"])] - best[r["request_id"]]) < 1e-9
    coverage = _independent_coverage(data_dir)
    rng = random.Random(11)
    sample = rng.sample(sorted(pairs), 300) + [(r["request_id"], r["source_programme_id"]) for r in reqs]
    for q, p in sample:
        assert abs(coverage(q, p) - pairs[(q, p)]) < 1e-6, (q, p)


def test_gold_best_applications(data_dir):
    rows = read(data_dir, "gold_best_applications")
    apps = {n["application_id"] for n in read(data_dir, "module_nodes") if n["application_id"]}
    assert {r["application_id"] for r in rows} <= apps
    items = {(n["tree_id"], n["node_id"]) for n in read(data_dir, "request_nodes") if n["node_kind"] == "item"}
    with_best = {(r["tree_id"], r["node_id"]) for r in rows if r["is_best"] == "true"}
    assert len(with_best) / len(items) > 0.9


# ---------------------------------------------------------------- BRD feasibility

def test_reference_kernel_reaches_brd_kpis(data_dir):
    """Feasibility guard for R3: the kernel semantics (HLD §4.6 with the §11
    amendments: catalogue default bounds for unqualified values, label agreement
    gating the value score, absolute tolerance for temperature), reviewed labels,
    stage 1 containment top-20 and stage 2 coverage must reach the BRD §6 targets.
    It is an oracle-parsed ceiling: values come from the gold attributes."""
    kpi = ref.kernel_kpis(data_dir)
    assert kpi["recall_at_20"] >= 0.90, kpi
    assert kpi["top1"] >= 0.85, kpi
    assert kpi["mean_regret"] <= 0.01, kpi
    assert kpi["mae_top_k"] <= 0.05, kpi
    assert kpi["mae"] <= 0.05, kpi
    assert kpi["unmeetable_flagged"] >= 0.95, kpi
    assert kpi["precision_at_0_9"] >= 0.95, kpi


def test_point_rule_for_unqualified_values_misses_the_kpis(data_dir):
    """Why default_bounds exists: treating ~30 % unqualified values as widened
    points (HLD §4.2) loses regret and MAE on this dataset."""
    kpi = ref.kernel_kpis(data_dir, default_bounds=None)
    assert kpi["mean_regret"] > 0.01 or kpi["mae_top_k"] > 0.05, kpi


def test_unreviewed_labels_show_documented_failure_mode(data_dir):
    """DoD §5.3: on unreviewed labels, stage-1 containment recalls the best candidate
    less often than weighted Jaccard; on reviewed labels it recalls it more often."""
    unrev = ref.stage1_recall(data_dir, "programme_class_multiset_unreviewed")
    rev = ref.stage1_recall(data_dir, "programme_class_multiset")
    assert unrev["containment"] < unrev["jaccard"], unrev
    assert rev["containment"] > rev["jaccard"], rev


# ---------------------------------------------------------------- customers

def test_customer_master(data_dir):
    cust = read(data_dir, "customers")
    assert 1800 <= len(cust) <= 2200
    gold = read(data_dir, "gold_customer_clusters")
    assert {g["record_id"] for g in gold} == {c["record_id"] for c in cust}
    clusters = Counter(g["cluster_id"] for g in gold)
    dups = sum(n - 1 for n in clusters.values() if n > 1)
    assert 0.06 <= dups / len(cust) <= 0.10
    kinds = {g["dup_kind"] for g in gold if g["dup_kind"]}
    assert {"spelling", "umlaut", "legal_form", "moved_plant", "shared_tax_id", "near_miss"} <= kinds
    assert len({c["group_name"] for c in cust if c["record_type"] == "group"}) == 8
    # every planted duplicate differs from its original
    rows = {c["record_id"]: [v for k, v in c.items() if k != "record_id"] for c in cust}
    for g in gold:
        if g["dup_kind"] and g["dup_kind"] != "near_miss":
            assert rows[g["record_id"]] != rows[g["cluster_id"]], g
    # a near miss looks like another record but is a different company
    cluster_of = {g["record_id"]: g["cluster_id"] for g in gold}
    by_id = {c["record_id"]: c for c in cust}
    near = [g for g in gold if g["dup_kind"] == "near_miss"]
    assert near
    for g in near:
        other = by_id[g["lookalike_of"]]
        assert cluster_of[g["record_id"]] != cluster_of[other["record_id"]]
        assert by_id[g["record_id"]]["street"] == other["street"]


# ---------------------------------------------------------------- neutrality

REAL_NAMES = [
    "BMW", "Volkswagen", "Audi", "Porsche", "Daimler", "Mercedes", "Opel", "Ford",
    "Toyota", "Honda", "Nissan", "Hyundai", "Kia", "Renault", "Peugeot", "Citroen", "Citroën", "Fiat",
    "Stellantis", "Volvo", "Tesla", "Skoda", "Škoda", "Lear", "Adient", "Faurecia", "Forvia",
    "Magna", "Brose", "Boshoku", "Tachi-S", "Grammer", "Recaro", "Johnson Controls",
    "Bosch", "Continental", "Webasto", "Gentherm", "Autoliv", "ZF",
]
NAME_RE = re.compile(r"\b(" + "|".join(re.escape(n) for n in REAL_NAMES) + r")\b")


def test_no_real_company_names(data_dir):
    paths = [p for p in sorted(data_dir.rglob("*")) if p.is_file()]
    paths += sorted((REPO / "scripts" / "src" / "scripts" / "coverage").glob("*.py"))
    if COMMITTED.exists():
        paths += [p for p in sorted(COMMITTED.rglob("*")) if p.is_file() and p.suffix != ".gz"]
    for p in paths:
        raw = p.read_bytes()
        text = gzip.decompress(raw).decode("utf-8") if p.suffix == ".gz" else raw.decode("utf-8")
        m = NAME_RE.search(text)
        assert m is None, (p.name, m and text[max(0, m.start() - 40):m.end() + 40])

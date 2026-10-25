"""Tests for the coverage-matching test dataset (automotive seating systems).

The generator is the specification of the benchmark, so these tests pin the
properties HANDOVER.md §3 asks for: determinism, size, leakage, noise rates,
vector calibration, oracle semantics and domain neutrality.
"""

import csv
import gzip
import hashlib
import io
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np
import pytest

from scripts.coverage import generate
from scripts.coverage import reference as ref

SEED = 7


def _run(out: Path) -> None:
    generate.main(["--seed", str(SEED), "--output", str(out)])


@pytest.fixture(scope="session")
def data_dir(tmp_path_factory) -> Path:
    out = tmp_path_factory.mktemp("coverage")
    _run(out)
    return out


def read(data_dir: Path, name: str) -> list[dict]:
    with gzip.open(data_dir / f"{name}.csv.gz", "rt", encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def vec(s: str) -> np.ndarray:
    return np.asarray(json.loads(s), dtype=np.float64)


def digest(root: Path) -> dict[str, str]:
    return {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


EXPECTED_FILES = [
    "catalogue_classes", "catalogue_attributes", "compatibility_rules",
    "internal_vocabulary", "module_nodes", "module_attributes",
    "programme_nodes", "programme_attributes", "request_nodes", "request_attributes",
    "programme_class_multiset", "programme_class_multiset_unreviewed",
    "phrasings", "unseen_requirements",
    "catalogue_vectors", "request_item_vectors", "vocabulary_vectors",
    "unseen_vectors", "attribute_name_vectors", "customers",
    "gold_item_class", "gold_vocabulary", "gold_request_attributes", "gold_unseen",
    "gold_requests", "gold_coverage", "gold_best_applications", "gold_customer_clusters",
]


# ---------------------------------------------------------------- files and size

def test_all_files_exist(data_dir):
    for name in EXPECTED_FILES:
        assert (data_dir / f"{name}.csv.gz").is_file(), name
    assert len(list((data_dir / "requests_json").glob("*.json"))) == len(read(data_dir, "gold_requests"))


def test_deterministic_byte_identical(data_dir, tmp_path):
    _run(tmp_path)
    assert digest(tmp_path) == digest(data_dir)


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
    # at most one attribute per dimension and class, so value matching is unambiguous
    dims = Counter((a["class_id"], a["dimension"]) for a in attrs)
    assert max(dims.values()) == 1


def test_units_used_in_requests(data_dir):
    texts = " ".join(a["value_text"] for a in read(data_dir, "request_attributes"))
    for unit in ["mm", "cm", "kN", " N", " W", " s", "min", "dB(A)", "kg", " A", "mA", "°C", "EUR"]:
        assert unit in texts, unit


# ---------------------------------------------------------------- module library

def test_module_library_or_tree(data_dir):
    nodes = read(data_dir, "module_nodes")
    by_kind = Counter(n["node_kind"] for n in nodes)
    roots = [n for n in nodes if n["parent_id"] == ""]
    configs = [n for n in nodes if n["node_kind"] == "variant_group" and n["parent_id"] != ""]
    apps = [n for n in nodes if n.get("application_id")]
    assert 35 <= len(roots) <= 45
    assert 80 <= len(configs) <= 100
    assert 140 <= len(apps) <= 160
    assert by_kind["item"] >= len(apps)
    # alternatives below a variant group are OR
    for n in nodes:
        if n["parent_id"] and any(p["node_id"] == n["parent_id"] and p["tree_id"] == n["tree_id"]
                                  and p["node_kind"] == "variant_group" for p in configs + roots):
            assert n["edge_type"] == "OR"


def test_combo_modules_cover_several_classes(data_dir):
    gold = {(g["tree_id"], g["node_id"]): g["class_id"] for g in read(data_dir, "gold_item_class")}
    nodes = read(data_dir, "module_nodes")
    classes_per_app = defaultdict(set)
    parent = {(n["tree_id"], n["node_id"]): n for n in nodes}
    for n in nodes:
        if n["node_kind"] == "item":
            app = n if n.get("application_id") else parent[(n["tree_id"], n["parent_id"])]
            classes_per_app[app["application_id"]].add(gold[(n["tree_id"], n["node_id"])])
    combos = [a for a, cs in classes_per_app.items() if len(cs) >= 2]
    combo_modules = {a.split(".")[0] for a in combos}
    assert 4 <= len(combo_modules) <= 5


# ---------------------------------------------------------------- programmes

def test_programmes(data_dir):
    nodes = read(data_dir, "programme_nodes")
    progs = {n["tree_id"] for n in nodes}
    assert 280 <= len(progs) <= 320
    items = [n for n in nodes if n["node_kind"] == "item"]
    with_module = {n["tree_id"] for n in items if n["module_ref"]}
    capability_only = progs - with_module
    assert 0.35 <= len(capability_only) / len(progs) <= 0.45
    makers = {n["source"] for n in nodes}
    assert len(makers) == 1  # internal archive is a single source


def test_misleading_wordings_touch_most_programmes(data_dir):
    vocab = read(data_dir, "gold_vocabulary")
    misleading = {(v["wording"], v["source"]) for v in vocab if v["is_misleading"] == "true"}
    assert 3 <= len(misleading) <= 4
    assert 45 <= len(vocab) <= 55
    for v in vocab:
        if v["is_misleading"] == "true":
            assert v["apparent_class_id"] != v["class_id"]
    nodes = read(data_dir, "programme_nodes")
    progs = {n["tree_id"] for n in nodes}
    touched = {n["tree_id"] for n in nodes if (n["label"], n["source"]) in misleading}
    assert len(touched) / len(progs) > 0.60


def test_programme_class_multiset_matches_items(data_dir):
    gold = {(g["tree_id"], g["node_id"]): g["class_id"] for g in read(data_dir, "gold_item_class")}
    expected = Counter()
    for n in read(data_dir, "programme_nodes"):
        if n["node_kind"] == "item":
            expected[(n["tree_id"], gold[(n["tree_id"], n["node_id"])])] += float(n["count"])
    got = {(r["entity_id"], r["feature_id"]): float(r["weight"])
           for r in read(data_dir, "programme_class_multiset")}
    assert got == dict(expected)


# ---------------------------------------------------------------- requests

def test_request_volume_and_hierarchy(data_dir):
    reqs = read(data_dir, "gold_requests")
    assert 75 <= len(reqs) <= 85
    nodes = read(data_dir, "request_nodes")
    items = [n for n in nodes if n["node_kind"] == "item"]
    assert 1350 <= len(items) <= 1650
    langs = Counter(r["lang"] for r in reqs)
    assert 0.4 <= langs["de"] / len(reqs) <= 0.6
    gran = Counter(r["granularity"] for r in reqs)
    assert gran["coarse"] > 0 and gran["detailed"] > 0
    # requirement depth varies
    parent = {(n["tree_id"], n["node_id"]): n["parent_id"] for n in nodes}

    def depth(t, nid):
        d = 0
        while parent[(t, nid)]:
            nid = parent[(t, nid)]
            d += 1
        return d

    depths = Counter(depth(n["tree_id"], n["node_id"]) for n in items)
    assert len(depths) >= 3
    # prose / notes are leaf containers
    assert any(n["node_kind"] == "container" and n["label"].lower().startswith(("note", "hinweis"))
               for n in nodes)


def test_stretched_and_unqualified_rates(data_dir):
    gold = read(data_dir, "gold_request_attributes")
    raw = {(a["tree_id"], a["node_id"], a["attr_id"]): a for a in read(data_dir, "request_attributes")}
    items = {(n["tree_id"], n["node_id"]) for n in read(data_dir, "request_nodes") if n["node_kind"] == "item"}
    stretched_items = {(g["tree_id"], g["node_id"]) for g in gold if g["stretched"] == "true"}
    assert 0.09 <= len(stretched_items) / len(items) <= 0.15
    numeric = [g for g in gold if g["category"] == ""]
    unqualified = [g for g in numeric
                   if not ref.has_bound_word(raw[(g["tree_id"], g["node_id"], g["attr_id"])]["value_text"])]
    assert 0.25 <= len(unqualified) / len(numeric) <= 0.35
    # some requirements state several values in one sentence
    per_item = Counter((g["tree_id"], g["node_id"]) for g in gold)
    assert sum(1 for c in per_item.values() if c >= 2) > 100


def test_some_requests_unmeetable(data_dir):
    reqs = read(data_dir, "gold_requests")
    unmeetable = [r for r in reqs if r["is_unmeetable"] == "true"]
    assert 5 <= len(unmeetable) <= 20
    for r in unmeetable:
        assert float(r["best_coverage"]) <= 0.90
    for r in reqs:
        if r["is_unmeetable"] == "false":
            assert float(r["best_coverage"]) > 0.90


def test_request_json_matches_tables(data_dir):
    nodes = read(data_dir, "request_nodes")
    attrs = read(data_dir, "request_attributes")
    for path in sorted((data_dir / "requests_json").glob("*.json"))[:10]:
        doc = json.loads(path.read_text(encoding="utf-8"))
        tid = doc["tree_id"]
        flat_nodes, flat_attrs = [], []

        def walk(n):
            flat_nodes.append(n["node_id"])
            flat_attrs.extend((n["node_id"], a["attr_id"]) for a in n.get("attributes", []))
            for c in n.get("children", []):
                walk(c)

        walk(doc["root"])
        assert sorted(flat_nodes) == sorted(n["node_id"] for n in nodes if n["tree_id"] == tid)
        assert sorted(flat_attrs) == sorted((a["node_id"], a["attr_id"]) for a in attrs if a["tree_id"] == tid)


# ---------------------------------------------------------------- phrasings / leakage

def test_phrasing_pool_split(data_dir):
    ph = read(data_dir, "phrasings")
    per_class = Counter((p["class_id"], p["split"]) for p in ph)
    classes = {p["class_id"] for p in ph}
    for c in classes:
        assert per_class[(c, "dev")] == 9 and per_class[(c, "test")] == 9
    dev = {p["text"].lower() for p in ph if p["split"] == "dev"}
    test = {p["text"].lower() for p in ph if p["split"] == "test"}
    assert not dev & test


def test_test_phrasings_never_in_development_data(data_dir):
    test = [p["text"] for p in read(data_dir, "phrasings") if p["split"] == "test"]
    labels = [n["label"] for n in read(data_dir, "request_nodes") if n["node_kind"] == "item"]
    pattern = re.compile("|".join(re.escape(t) for t in sorted(test, key=len, reverse=True)), re.I)
    leaks = [lab for lab in labels if pattern.search(lab)]
    assert not leaks, leaks[:5]
    unseen = read(data_dir, "unseen_requirements")
    assert 180 <= len(unseen) <= 220
    assert all(pattern.search(u["text"]) for u in unseen)


# ---------------------------------------------------------------- vectors

def test_vectors_are_normalised_64d(data_dir):
    for name, col in [("catalogue_vectors", "vec"), ("request_item_vectors", "vec"),
                      ("vocabulary_vectors", "vec"), ("unseen_vectors", "vec"),
                      ("attribute_name_vectors", "vec")]:
        for row in read(data_dir, name)[:200]:
            v = vec(row[col])
            assert v.shape == (64,)
            assert abs(np.linalg.norm(v) - 1) < 2e-3
    for row in read(data_dir, "request_item_vectors")[:200]:
        toks = json.loads(row["tokens"])
        assert 3 <= len(toks) <= 12 and all(len(t) == 64 for t in toks)


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
    gold = {(g["tree_id"], g["node_id"]): g["class_id"] for g in read(data_dir, "gold_item_class")}
    class_ids, C, _, X, _, src, y = _classification_inputs(
        data_dir, "request_item_vectors", ["tree_id", "node_id"], gold)
    raw = ref.top1_accuracy(ref.centroid_scores(X, C), class_ids, y)
    centred = ref.top1_accuracy(ref.centroid_scores(ref.centre_by_source(X, src), C), class_ids, y)
    assert 0.50 <= raw <= 0.70, raw
    assert 0.80 <= centred <= 0.92, centred


def test_maxsim_beats_centroid_on_held_out_phrasings(data_dir):
    gold = {(g["item_id"],): g["class_id"] for g in read(data_dir, "gold_unseen")}
    class_ids, C, E, X, T, src, y = _classification_inputs(
        data_dir, "unseen_vectors", ["item_id"], gold)
    centroid = ref.top1_accuracy(ref.centroid_scores(ref.centre_by_source(X, src), C), class_ids, y)
    late = ref.top1_accuracy(ref.maxsim_scores(T, E), class_ids, y)
    assert late >= centroid + 0.08, (centroid, late)
    assert late >= 0.85, late


def test_misleading_wordings_sit_near_wrong_class(data_dir):
    gv = {(v["wording"], v["source"]): v for v in read(data_dir, "gold_vocabulary")}
    cat = read(data_dir, "catalogue_vectors")
    class_ids = [c["class_id"] for c in cat]
    C = np.stack([vec(c["vec"]) for c in cat])
    for row in read(data_dir, "vocabulary_vectors"):
        g = gv[(row["wording"], row["source"])]
        if g["is_misleading"] == "true":
            best = class_ids[int(np.argmax(C @ vec(row["vec"])))]
            assert best == g["apparent_class_id"], row["wording"]


def test_attribute_name_cosine_floor(data_dir):
    rows = read(data_dir, "attribute_name_vectors")
    V = np.stack([vec(r["vec"]) for r in rows])
    sims = V @ V.T
    off = sims[~np.eye(len(V), dtype=bool)]
    assert 0.80 <= float(np.median(off)) <= 0.90


# ---------------------------------------------------------------- oracle

def test_oracle_direction_aware_rules():
    # at_most: lower is better
    assert ref.oracle_satisfies("at_most", 38.0, 38.0, 35.0, 35.0)
    assert not ref.oracle_satisfies("at_most", 38.0, 38.0, 41.0, 41.0)
    # at_least: higher is better
    assert ref.oracle_satisfies("at_least", 240.0, 240.0, 250.0, 250.0)
    assert not ref.oracle_satisfies("at_least", 240.0, 240.0, 230.0, 230.0)
    # within: the provided range must cover the required range
    assert ref.oracle_satisfies("within", 15.0, 25.0, 10.0, 30.0)
    assert not ref.oracle_satisfies("within", 15.0, 25.0, 20.0, 30.0)
    # equals: relative tolerance 2 %
    assert ref.oracle_satisfies("equals", 200.0, 200.0, 203.0, 203.0)
    assert not ref.oracle_satisfies("equals", 200.0, 200.0, 210.0, 210.0)


def test_gold_coverage_complete_and_consistent(data_dir):
    reqs = read(data_dir, "gold_requests")
    progs = {n["tree_id"] for n in read(data_dir, "programme_nodes")}
    cov = read(data_dir, "gold_coverage")
    assert len(cov) == len(reqs) * len(progs)
    best = defaultdict(float)
    for c in cov:
        v = float(c["true_coverage"])
        assert 0.0 <= v <= 1.0
        best[c["query_id"]] = max(best[c["query_id"]], v)
    for r in reqs:
        assert abs(best[r["request_id"]] - float(r["best_coverage"])) < 1e-9


def test_gold_best_applications(data_dir):
    rows = read(data_dir, "gold_best_applications")
    items = {(n["tree_id"], n["node_id"]) for n in read(data_dir, "request_nodes") if n["node_kind"] == "item"}
    with_best = {(r["tree_id"], r["node_id"]) for r in rows if r["is_best"] == "true"}
    assert len(with_best) / len(items) > 0.9


def test_reference_kernel_reaches_brd_kpis_with_gold_labels(data_dir):
    """Feasibility guard for R3: the HLD scoring with hard gold labels and
    template bounds must reach the BRD §6 targets on this dataset."""
    kpi = ref.kernel_kpis(data_dir)
    assert kpi["top1"] >= 0.85, kpi
    assert kpi["mean_regret"] <= 0.01, kpi
    assert kpi["mae"] <= 0.05, kpi
    assert kpi["unmeetable_flagged"] >= 0.95, kpi


# ---------------------------------------------------------------- customers

def test_customer_master(data_dir):
    cust = read(data_dir, "customers")
    assert 1800 <= len(cust) <= 2200
    clusters = Counter(c["cluster_id"] for c in read(data_dir, "gold_customer_clusters"))
    dups = sum(n - 1 for n in clusters.values() if n > 1)
    assert 0.06 <= dups / len(cust) <= 0.10
    kinds = {c["dup_kind"] for c in cust if c["dup_kind"]}
    assert {"spelling", "umlaut", "legal_form", "moved_plant", "shared_tax_id", "near_miss"} <= kinds
    groups = {c["group_name"] for c in cust if c["record_type"] == "group"}
    assert len({g for g in groups}) >= 8


# ---------------------------------------------------------------- neutrality

REAL_NAMES = [
    "BMW", "Volkswagen", "VW ", "Audi", "Porsche", "Daimler", "Mercedes", "Opel", "Ford",
    "Toyota", "Honda", "Nissan", "Hyundai", "Kia", "Renault", "Peugeot", "Citroen", "Fiat",
    "Stellantis", "Volvo", "Tesla", "Skoda", "SEAT", "Lear", "Adient", "Faurecia", "Forvia",
    "Magna", "Brose", "Toyota Boshoku", "Tachi-S", "Grammer", "Recaro", "Johnson Controls",
    "Bosch", "Continental", "Webasto", "Gentherm", "Autoliv", "ZF ",
]


def test_no_real_company_names(data_dir):
    pat = re.compile(r"\b(" + "|".join(re.escape(n.strip()) for n in REAL_NAMES) + r")\b")
    for p in sorted(data_dir.rglob("*")):
        if not p.is_file():
            continue
        raw = p.read_bytes()
        text = gzip.decompress(raw).decode("utf-8") if p.suffix == ".gz" else raw.decode("utf-8")
        m = pat.search(text)
        assert m is None, (p.name, m and text[max(0, m.start() - 40):m.end() + 40])

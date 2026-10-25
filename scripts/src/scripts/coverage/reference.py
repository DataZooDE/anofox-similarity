"""Reference semantics for the coverage benchmark.

Three things live here:

* the **oracle**: strict, direction-aware satisfaction with the true bound,
  used to compute the gold coverage;
* a **reference pivot classifier** (centroid with per-source centring, z-score,
  late-interaction MaxSim) used to calibrate the synthetic vectors;
* a **reference kernel** that follows HLD §4.2 / §4.6 literally, used as a
  feasibility guard: if it cannot reach the BRD §6 KPIs on the dataset, the
  C++ kernel cannot either.

The reference reads only the published dataset files, never generator
internals, so it doubles as a check that the files carry what a user needs.
"""

import csv
import gzip
import itertools
import json
import math
import re
from collections import defaultdict
from pathlib import Path

import numpy as np

from .domain import LINEAR_DIMENSIONS, ORDINALS

AFFINE_DIMENSIONS = {"temperature"}

EQUALS_TOL = 0.02

# --------------------------------------------------------------------------- oracle


def oracle_satisfies(bound: str, r_lo: float, r_hi: float, p_lo: float, p_hi: float) -> bool:
    """Does the provided interval [p_lo, p_hi] satisfy the requirement?

    at_least v: p_hi >= v; at_most v: p_lo <= v; within [lo, hi]: the provided
    range covers the required range; equals v: |p - v| <= 2 % of |v|.
    """
    if bound == "at_least":
        return p_hi >= r_lo
    if bound == "at_most":
        return p_lo <= r_hi
    if bound == "within":
        return p_lo <= r_lo and p_hi >= r_hi
    if bound == "equals":
        return abs(p_lo - r_lo) <= EQUALS_TOL * abs(r_lo) and abs(p_hi - r_lo) <= EQUALS_TOL * abs(r_lo)
    raise ValueError(f"unknown bound {bound!r}")


def oracle_assignment_coverage(req_items, cand_items, share) -> float:
    """Coverage with one-to-one use of offer items (HANDOVER §7.3).

    req_items:  [(id, class, count, attrs)]; cand_items: [(id, class, count, values)];
    share(attrs, values) -> share of the attributes the values satisfy (1 if no attrs).
    Every offer item of a class serves exactly one request item of that class; the
    assignment that maximises the total is taken. A request item scores
    max_{c in block} share * min(1, sum of block counts / its count), 0 for an empty block.
    """
    if not req_items:
        return 0.0
    by_cls_q, by_cls_c = defaultdict(list), defaultdict(list)
    for q in req_items:
        by_cls_q[q[1]].append(q)
    for c in cand_items:
        by_cls_c[c[1]].append(c)
    total = 0.0
    for cls, qs in by_cls_q.items():
        cs = by_cls_c.get(cls, [])
        shares = [[share(q[3], c[3]) for c in cs] for q in qs]
        best = 0.0
        for choice in itertools.product(range(len(qs)), repeat=len(cs)):
            s = 0.0
            for qi, q in enumerate(qs):
                block = [ci for ci, a in enumerate(choice) if a == qi]
                if block:
                    s += max(shares[qi][ci] for ci in block) * min(1.0, sum(cs[ci][2] for ci in block) / q[2])
            best = max(best, s)
        total += best
    return total / len(req_items)


def ordinal_rank(dimension: str, category: str) -> int:
    return ORDINALS[dimension].index(category)


def ordinal_satisfies(dimension: str, required: str, provided: str) -> bool:
    if not provided:
        return False
    return ordinal_rank(dimension, provided) >= ordinal_rank(dimension, required)


# --------------------------------------------------------------------------- pivot reference


def normalise_rows(X: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(X, axis=1, keepdims=True)
    return X / np.where(n == 0, 1, n)


def centre_by_source(X: np.ndarray, sources: list[str]) -> np.ndarray:
    X = np.asarray(X, dtype=np.float64)
    out = np.empty_like(X)
    src = np.asarray(sources)
    for s in sorted(set(sources)):
        m = src == s
        out[m] = X[m] - X[m].mean(axis=0)
    return normalise_rows(out)


def centroid_scores(X: np.ndarray, C: np.ndarray) -> np.ndarray:
    return normalise_rows(X) @ normalise_rows(C).T


def zscore(S: np.ndarray) -> np.ndarray:
    sd = S.std(axis=1, keepdims=True)
    return (S - S.mean(axis=1, keepdims=True)) / np.where(sd == 0, 1, sd)


def maxsim(q: np.ndarray, d: np.ndarray) -> float:
    """Mean over query tokens of the best cosine against document tokens."""
    return float((normalise_rows(q) @ normalise_rows(d).T).max(axis=1).mean())


def maxsim_scores(T: list[np.ndarray], E: list[np.ndarray]) -> np.ndarray:
    return np.array([[maxsim(t, e) for e in E] for t in T])


def top1_accuracy(S: np.ndarray, class_ids: list[str], gold: list[str]) -> float:
    pred = [class_ids[i] for i in np.argmax(S, axis=1)]
    return float(np.mean([p == g for p, g in zip(pred, gold)]))


# --------------------------------------------------------------------------- HLD value rules

LOG_TOL = 0.02  # decades
LIN_TOL = 0.02  # relative
SOFT_BAND = 0.05
POINT_WIDEN = 0.01  # decades


def _ell(lo: float, hi: float) -> tuple[float, float]:
    return math.log10(lo), math.log10(hi)


AFFINE_TOL, AFFINE_BAND = 1.0, 2.5  # K; amended 2026-10-25 (HLD §11)


def value_satisfies(bound: str | None, r_lo: float, r_hi: float, p_lo: float, p_hi: float,
                    scale: str, affine: bool = False) -> tuple[float, float]:
    """HLD §4.2 `value_satisfies`: (score, distance). bound None = unqualified point.

    Affine dimensions (temperature) use an absolute tolerance and band in the base
    unit (K), and the distance is absolute (K)."""
    if affine:
        def soft_abs(d: float) -> float:
            return 1.0 if d <= AFFINE_TOL else max(0.0, 1.0 - (d - AFFINE_TOL) / AFFINE_BAND)
        if bound == "at_least":
            d = max(0.0, r_lo - p_hi)
            return soft_abs(d), d
        if bound == "at_most":
            d = max(0.0, p_lo - r_hi)
            return soft_abs(d), d
        if bound == "equals":
            d = min(abs(r_lo - p_lo), abs(r_lo - p_hi))
            return soft_abs(d), d
        lo, hi = (r_lo, r_hi) if bound == "within" else (r_lo - AFFINE_TOL, r_hi + AFFINE_TOL)
        width = hi - lo
        inter = max(0.0, min(hi, p_hi + AFFINE_TOL) - max(lo, p_lo - AFFINE_TOL))
        share = 1.0 if width <= 0 else inter / width
        return share, 1.0 - share
    log = scale == "log" and r_lo > 0 and p_lo > 0
    tol_rel = (1 - 10 ** -LOG_TOL) if log else LIN_TOL

    def soften(d: float) -> float:
        if d <= tol_rel:
            return 1.0
        return max(0.0, 1.0 - (d - tol_rel) / SOFT_BAND)

    if bound == "at_least":
        d = max(0.0, (r_lo - p_hi) / abs(r_lo))
        return soften(d), d
    if bound == "at_most":
        d = max(0.0, (p_lo - r_hi) / abs(r_hi))
        return soften(d), d
    if bound == "equals":
        p = p_lo if abs(p_lo - r_lo) >= abs(p_hi - r_lo) else p_hi
        d = abs(r_lo - p) / abs(r_lo)
        return soften(d), d
    if bound is None:  # unqualified point: within [v, v] widened
        if log:
            r_lo, r_hi = r_lo * 10 ** -POINT_WIDEN, r_hi * 10 ** POINT_WIDEN
        else:
            r_lo, r_hi = r_lo * (1 - LIN_TOL / 2), r_hi * (1 + LIN_TOL / 2)
        bound = "within"
    if bound == "within":
        if log:
            a, b = _ell(r_lo, r_hi)
            # tolerance widens the provided range by LOG_TOL decades on each side
            c, e = math.log10(p_lo) - LOG_TOL, math.log10(p_hi) + LOG_TOL
        else:
            a, b = r_lo, r_hi
            c, e = p_lo - LIN_TOL * abs(p_lo), p_hi + LIN_TOL * abs(p_hi)
        width = b - a
        inter = max(0.0, min(b, e) - max(a, c))
        share = 1.0 if width <= 0 and c <= a <= e else (inter / width if width > 0 else 0.0)
        return share, 1.0 - share
    raise ValueError(bound)


def compat(dimension: str, required: str, provided: str, rules: dict) -> float:
    if not provided:
        return 0.0
    return rules.get((dimension, required, provided), 1.0 if required == provided else 0.0)


# --------------------------------------------------------------------------- dataset access


def _read(data_dir: Path, name: str) -> list[dict]:
    with gzip.open(Path(data_dir) / f"{name}.csv.gz", "rt", encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


BOUND_RE = re.compile(
    r"(at least|at most|min\.|max\.|minimum|maximum|up to|not more than|not less than|or better|±|nominal|Nennwert|"
    r"mindestens|höchstens|bis zu|maximal|minimal|nicht mehr als|nicht weniger als|oder besser|≥|≤|"
    r"\d\s*(–|…|to|bis)\s*\d|between|zwischen)", re.I)


def has_bound_word(text: str) -> bool:
    return bool(BOUND_RE.search(text))


def _scale(dimension: str) -> str:
    return "linear" if dimension in LINEAR_DIMENSIONS or dimension in ORDINALS else "log"


class _Data:
    """Request and programme trees, flattened to items with attributes."""

    def __init__(self, data_dir: Path):
        gold_cls = {(g["tree_id"], g["node_id"]): g["class_id"] for g in _read(data_dir, "gold_item_class")}
        raw_text = {(a["tree_id"], a["node_id"], a["attr_id"]): (a["name"], a["value_text"])
                    for a in _read(data_dir, "request_attributes")}
        self.req_items = defaultdict(list)  # rid -> [(nid, class, count)]
        for n in _read(data_dir, "request_nodes"):
            if n["node_kind"] == "item":
                self.req_items[n["tree_id"]].append((n["node_id"], gold_cls[(n["tree_id"], n["node_id"])],
                                                     float(n["count"])))
        self.req_attrs = defaultdict(list)
        for g in _read(data_dir, "gold_request_attributes"):
            name, text = raw_text[(g["tree_id"], g["node_id"], g["attr_id"])]
            g = dict(g, name=name, qualified=has_bound_word(text))
            self.req_attrs[(g["tree_id"], g["node_id"])].append(g)
        self.prog_items = defaultdict(list)  # pid -> [(nid, count)]
        self.prog_label = {}
        for n in _read(data_dir, "programme_nodes"):
            if n["node_kind"] == "item":
                self.prog_items[n["tree_id"]].append((n["node_id"], float(n["count"])))
                self.prog_label[(n["tree_id"], n["node_id"])] = (n["label"], n["source"])
        self.gold_cls = gold_cls
        self.prog_attrs = defaultdict(list)
        for a in _read(data_dir, "programme_attributes"):
            self.prog_attrs[(a["tree_id"], a["node_id"])].append(a)
        self.rules = {(r["dimension"], r["required"], r["provided"]): float(r["score"])
                      for r in _read(data_dir, "compatibility_rules")}
        names = _read(data_dir, "attribute_name_vectors")
        V = np.stack([np.asarray(json.loads(r["vec"])) for r in names])
        self.name_idx = {r["name"]: i for i, r in enumerate(names)}
        self.name_sims = V @ V.T
        off = self.name_sims[~np.eye(len(V), dtype=bool)]
        self.name_floor = float(np.median(off))
        self.gold_cov = defaultdict(dict)
        for c in _read(data_dir, "gold_coverage"):
            self.gold_cov[c["query_id"]][c["candidate_id"]] = float(c["true_coverage"])
        self.requests = _read(data_dir, "gold_requests")

    def s_name(self, a: str, b: str) -> float:
        i, j = self.name_idx.get(a), self.name_idx.get(b)
        if i is None or j is None:
            return 0.0
        cos = self.name_sims[i, j]
        return float(np.clip((cos - self.name_floor) / (1 - self.name_floor), 0, 1))


def _multiset(data_dir: Path, name: str) -> dict:
    out = defaultdict(dict)
    for r in _read(data_dir, name):
        out[r["entity_id"]][r["feature_id"]] = float(r["weight"])
    return out


def _weighted(q: dict, c: dict) -> tuple[float, float]:
    keys = set(q) | set(c)
    smin = sum(min(q.get(k, 0), c.get(k, 0)) for k in keys)
    smax = sum(max(q.get(k, 0), c.get(k, 0)) for k in keys)
    sq = sum(q.values())
    return (smin / sq if sq else 0.0), (smin / smax if smax else 0.0)


def _query_multiset(d: _Data, rid: str) -> dict:
    q = defaultdict(float)
    for _, cls, n in d.req_items[rid]:
        q[cls] += n
    return dict(q)


def stage1_rank(d: _Data, ms: dict, rid: str, mode: str, tie_break: float = 0.01) -> list[tuple[str, float]]:
    q = _query_multiset(d, rid)
    scored = []
    for pid in sorted(ms):
        cont, jac = _weighted(q, ms[pid])
        s = cont + tie_break * jac if mode == "containment" else jac
        scored.append((pid, s))
    scored.sort(key=lambda t: (-t[1], t[0]))
    return scored


def stage1_recall(data_dir: Path, multiset_name: str, k: int = 20) -> dict:
    """Recall@k of a gold-best candidate for stage-1 ranking alone, per mode."""
    d = _Data(data_dir)
    ms = _multiset(data_dir, multiset_name)
    out = {}
    for mode in ("containment", "jaccard"):
        hits = []
        for r in d.requests:
            rid, best = r["request_id"], float(r["best_coverage"])
            hits.append(any(d.gold_cov[rid][p] >= best - 1e-9 for p, _ in stage1_rank(d, ms, rid, mode)[:k]))
        out[mode] = float(np.mean(hits))
    return out


# --------------------------------------------------------------------------- HLD kernel

# lambda: label credit. Amended 2026-10-25 (HLD §11): the item score is
# lambda*L + (1-lambda)*L*a, default lambda 0, so label agreement gates the value
# score instead of adding credit when values fail.
LAMBDA, MU = 0.0, 0.2
PARTITION_GATE, COUNT_GATE = 0.3, 0.5
STAGE1_WEIGHT = 0.05


def _attr_score(d: _Data, x: dict, y: dict, default_bounds: str | None = "catalogue") -> float:
    if x["dimension"] != y["dimension"]:
        return 0.0  # dimension gate
    name = MU * d.s_name(x["name"], y["name"]) + (1 - MU)
    if x["category"]:
        return name * compat(x["dimension"], x["category"], y["category"], d.rules)
    # an unqualified value takes the template bound (default_bounds) or is a widened point (HLD)
    bound = x["bound"] if x["qualified"] or default_bounds == "catalogue" else None
    score, _ = value_satisfies(bound, float(x["lo"]), float(x["hi"]), float(y["lo"]), float(y["hi"]),
                               _scale(x["dimension"]), affine=x["dimension"] in AFFINE_DIMENSIONS)
    return name * score


def _a(d: _Data, rid: str, q_nid: str, pid: str, c_nid: str, default_bounds: str | None = "catalogue") -> float:
    xs = d.req_attrs[(rid, q_nid)]
    if not xs:
        return 1.0
    ys = d.prog_attrs[(pid, c_nid)]
    return sum(max((_attr_score(d, x, y, default_bounds) for y in ys), default=0.0) for x in xs) / len(xs)


def kernel_coverage(d: _Data, rid: str, pid: str, label_of, default_bounds: str | None = "catalogue") -> float:
    """Partition assignment with hard labels (reviewed candidates, HLD `auto`).

    Each candidate item goes to q*(c) = argmax_q L(q, c) if L >= partition_gate. Ties
    between request items (e.g. driver and passenger seat) go to the tied request
    item with the largest remaining count deficit, then the lowest node id, so one
    offer item serves one request block (HANDOVER §7.3).
    """
    q_items = d.req_items[rid]
    blocks = defaultdict(list)
    need = {q_nid: n_q for q_nid, _, n_q in q_items}
    for c_nid, n_c in sorted(d.prog_items[pid]):
        c_cls = label_of(pid, c_nid)
        ls = [(1.0 if cls == c_cls else 0.0, q_nid) for q_nid, cls, _ in q_items]
        best = max((l for l, _ in ls), default=0.0)
        if best < PARTITION_GATE:
            continue
        tied = [q for l, q in ls if l == best]
        q_star = min(tied, key=lambda q: (-need[q], q))
        need[q_star] -= n_c
        blocks[q_star].append((c_nid, n_c, best))
    scores = []
    for q_nid, _, n_q in q_items:
        block = blocks.get(q_nid, [])
        if not block:
            scores.append(0.0)
            continue
        best = max(LAMBDA * l + (1 - LAMBDA) * l * _a(d, rid, q_nid, pid, c, default_bounds) for c, _, l in block)
        count = min(1.0, sum(n for _, n, l in block if l >= COUNT_GATE) / n_q)
        scores.append(best * count)
    return sum(scores) / len(scores)


def kernel_kpis(data_dir: Path, k: int = 20, default_bounds: str | None = "catalogue") -> dict:
    """BRD §6 KPIs of the reference kernel, an oracle-parsed ceiling: values come from
    the gold attributes, not from parsing value_text.

    default_bounds='catalogue' gives unqualified values the bound of their catalogue
    template (HLD §11 amendment); None treats them as widened points (HLD §4.2)."""
    d = _Data(data_dir)
    ms = _multiset(data_dir, "programme_class_multiset")

    def label_of(pid, nid):
        return d.gold_cls[(pid, nid)]

    recall, top1, regret, errs, errs_k, flagged, accepted = [], [], [], [], [], [], []
    for r in d.requests:
        rid, best = r["request_id"], float(r["best_coverage"])
        gold = d.gold_cov[rid]
        ranked = stage1_rank(d, ms, rid, "containment")[:k]
        recall.append(any(gold[p] >= best - 1e-9 for p, _ in ranked))
        rescored = []
        for pid, s1 in ranked:
            cov = kernel_coverage(d, rid, pid, label_of, default_bounds)
            rescored.append((cov + STAGE1_WEIGHT * s1, cov, pid))
            errs_k.append(abs(cov - gold[pid]))
        rescored.sort(key=lambda t: (-t[0], t[2]))
        _, cov1, p1 = rescored[0]
        top1.append(gold[p1] >= best - 1e-9)
        errs.append(abs(cov1 - gold[p1]))
        regret.append(best - gold[p1])
        if r["is_unmeetable"] == "true":
            flagged.append(cov1 < 0.95)
        if cov1 >= 0.9:
            accepted.append(gold[p1] >= 0.9)
    return {
        "recall_at_20": float(np.mean(recall)),
        "top1": float(np.mean(top1)),
        "mean_regret": float(np.mean(regret)),
        # MAE of the top-1 coverage and over all evaluated top-k pairs (the gated KPI)
        "mae": float(np.mean(errs)),
        "mae_top_k": float(np.mean(errs_k)),
        "unmeetable_flagged": float(np.mean(flagged)) if flagged else 1.0,
        "precision_at_0_9": float(np.mean(accepted)) if accepted else 1.0,
        "auto_accept_share": len(accepted) / len(d.requests),
    }

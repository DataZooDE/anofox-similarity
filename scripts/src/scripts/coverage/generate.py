"""Generate the coverage-matching benchmark: automotive seating systems.

    uv run generate-coverage-data --seed 7 --output test/data/coverage/

A fictional seating supplier answers seat-system specifications from fictional
vehicle makers with modules from its library and reuses past seat programmes.
The output is deterministic: the same seed gives byte-identical files.
See test/data/coverage/README.md for the schema and the oracle semantics.
"""

import argparse
import csv
import gzip
import io
import json
import math
import zlib
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from . import customers as customers_mod
from . import vectors as vectors_mod
from .domain import (BASE_UNIT, CLASS_BY_ID, CLASSES, COMBOS, FRAMES, GROUP_LABELS, INTERNAL_SOURCE,
                     MAKERS, ORDINALS, PHRASINGS, UNITS, VOCABULARY, AttrTemplate, from_base, to_base)
from .reference import oracle_satisfies, ordinal_satisfies

N_PROGRAMMES = 300
N_REQUESTS = 80
N_UNMEETABLE = 10
N_UNSEEN = 200
CAPABILITY_ONLY_SHARE = 0.4
UNQUALIFIED_SHARE = 0.28  # of at_least / at_most values; within is always a range


def rng_for(seed: int, name: str) -> np.random.Generator:
    return np.random.Generator(np.random.PCG64([seed, zlib.crc32(name.encode())]))


def pick(rng, seq):
    return seq[int(rng.integers(len(seq)))]


# --------------------------------------------------------------------------- number formatting


def fmt_num(x: float) -> str:
    """Canonical float for CSV: shortest round-trip repr of a 10-significant-digit value."""
    if x is None:
        return ""
    v = float(f"{x:.10g}")
    if v == int(v) and abs(v) < 1e15:
        return str(int(v))
    return repr(v)


def round_to(x: float, step: float, mode: str) -> float:
    q = x / step
    q = math.floor(q + 1e-9) if mode == "down" else math.ceil(q - 1e-9) if mode == "up" else round(q)
    return round(q * step, 10)


def sig_round(x: float, digits: int, mode: str) -> float:
    if x == 0:
        return 0.0
    step = 10 ** (math.floor(math.log10(abs(x))) - digits + 1)
    return round_to(x, step, mode)


def render_number(x: float, lang: str, thousands: bool = False) -> str:
    v = float(f"{x:.6g}")
    s = str(int(v)) if v == int(v) else f"{v:.6g}"
    if "e" in s:
        s = f"{v:f}".rstrip("0").rstrip(".")
    if thousands and v == int(v) and abs(v) >= 10000:
        s = f"{int(v):,}"
        if lang == "de":
            s = s.replace(",", ".")
    elif lang == "de":
        s = s.replace(".", ",")
    return s


def unit_step(unit: str, tmpl: AttrTemplate) -> float:
    """A sensible rendering step in a customer unit."""
    if unit == tmpl.natural_unit:
        return tmpl.step
    # convert the natural step into the customer unit and snap to 1/2/5 x 10^n
    base_step = to_base(tmpl.step, tmpl.natural_unit) - to_base(0, tmpl.natural_unit)
    s = base_step / UNITS[unit][1]
    e = math.floor(math.log10(s))
    m = s / 10 ** e
    m = 1 if m < 1.5 else 2 if m < 3.5 else 5 if m < 7.5 else 10
    return m * 10 ** e


# --------------------------------------------------------------------------- data model


@dataclass
class Node:
    tree_id: str
    node_id: str
    parent_id: str
    node_kind: str
    edge_type: str
    label: str
    context: str = ""
    count: float = 1.0
    source: str = ""
    extra: dict = field(default_factory=dict)


@dataclass
class Attr:
    tree_id: str
    node_id: str
    attr_id: str
    name: str
    dimension: str
    unit: str
    lo: float | None
    hi: float | None
    role: str
    bound: str = ""
    category: str = ""


@dataclass
class Item:
    """A classified item with provided values, as stored in the library or archive."""
    class_id: str
    count: float
    values: dict  # attr key -> (lo, hi) in base units, or category string


# --------------------------------------------------------------------------- value models


def sample_value(rng, tmpl: AttrTemplate, centre: float | None = None):
    """Provided value of an application, in base units (or a category)."""
    if tmpl.is_ordinal:
        scale = ORDINALS[tmpl.dimension]
        return scale[int(rng.choice(len(scale), p=[0.15, 0.35, 0.35, 0.15]))]
    lo, hi = tmpl.lo, tmpl.hi
    if tmpl.bound == "within":
        span = hi - lo
        a = round_to(lo + rng.uniform(0.0, 0.35) * span, tmpl.step, "near")
        b = round_to(hi - rng.uniform(0.0, 0.35) * span, tmpl.step, "near")
        return (to_base(a, tmpl.natural_unit), to_base(b, tmpl.natural_unit))
    c = centre if centre is not None else rng.uniform(lo, hi)
    v = min(hi, max(lo, c * rng.uniform(0.9, 1.1)))
    v = round_to(v, tmpl.step, "near")
    if tmpl.dimension == "count" and tmpl.key == "ways":
        v = max(lo, round_to(v, 2, "near"))
    b = to_base(v, tmpl.natural_unit)
    return (b, b)


def sample_count(rng, cls) -> float:
    if cls.max_count == 1:
        return 1.0
    options = {"seat_heating": [2, 4], "seat_ventilation": [1, 2], "massage": [2, 4, 6, 8],
               "seat_motor": [2, 3, 4, 6]}[cls.class_id]
    return float(pick(rng, options))


# --------------------------------------------------------------------------- module library


def build_modules(seed: int):
    rng = rng_for(seed, "modules")
    wordings = defaultdict(list)
    for w in VOCABULARY:
        wordings[w.class_id].append(w)
    plain = {c: [w.text for w in ws if not w.misleading] for c, ws in wordings.items()}
    misleading = {w.class_id: w.text for w in VOCABULARY if w.misleading}

    second = ["seat_track", "seat_heating", "recliner", "trim_cover", "foam_pad", "height_adjuster",
              "headrest_powered", "wiring"]
    plans = []  # (classes, label, item wordings per class)
    for cls in CLASSES:
        plans.append(((cls.class_id,), plain[cls.class_id][0], {cls.class_id: plain[cls.class_id][0]}))
    for cid in second:
        plans.append(((cid,), plain[cid][1], {cid: plain[cid][1]}))
    for cid, text in sorted(misleading.items()):
        plans.append(((cid,), text, {cid: text}))
    for label, classes in COMBOS:
        plans.append((classes, label, {c: plain[c][0] for c in classes}))

    nodes, attrs, apps = [], [], {}  # apps: application_id -> list of (item node id, Item)
    n_configs = 0
    for i, (classes, label, item_wording) in enumerate(plans, start=1):
        tid = f"M{i:02d}"
        nodes.append(Node(tid, tid, "", "variant_group", "AND", label, source=INTERNAL_SOURCE))
        centres = {(c, t.key): rng.uniform(t.lo, t.hi) for c in classes for t in CLASS_BY_ID[c].attrs}
        k_conf = 3 if i % 4 == 0 else 2
        for j in range(1, k_conf + 1):
            n_configs += 1
            cid_ = f"{tid}.C{j}"
            nodes.append(Node(tid, cid_, tid, "variant_group", "OR", f"{label} Ausführung {j}",
                              source=INTERNAL_SOURCE))
            # shift the configuration up or down the range
            shift = rng.uniform(0.85, 1.15)
            k_app = 2 if (i + j) % 3 != 0 else 1
            for k in range(1, k_app + 1):
                aid = f"{cid_}.A{k}"
                items = []
                for c in classes:
                    cls = CLASS_BY_ID[c]
                    vals = {t.key: sample_value(rng, t, centres[(c, t.key)] * shift) for t in cls.attrs}
                    items.append(Item(c, sample_count(rng, cls), vals))
                if len(items) == 1:
                    nodes.append(Node(tid, aid, cid_, "item", "OR", item_wording[classes[0]],
                                      count=items[0].count, source=INTERNAL_SOURCE,
                                      extra={"application_id": aid, "class_id": classes[0]}))
                    apps[aid] = [(aid, items[0])]
                else:
                    nodes.append(Node(tid, aid, cid_, "container", "OR", f"{label} {j}.{k}",
                                      source=INTERNAL_SOURCE, extra={"application_id": aid}))
                    apps[aid] = []
                    for it in items:
                        nid = f"{aid}.{it.class_id}"
                        nodes.append(Node(tid, nid, aid, "item", "AND", item_wording[it.class_id],
                                          count=it.count, source=INTERNAL_SOURCE, extra={"class_id": it.class_id}))
                        apps[aid].append((nid, it))
                for nid, it in apps[aid]:
                    attrs.extend(provided_attrs(tid, nid, it))
    return nodes, attrs, apps


def provided_attrs(tid: str, nid: str, it: Item) -> list[Attr]:
    out = []
    for t in CLASS_BY_ID[it.class_id].attrs:
        v = it.values[t.key]
        if t.is_ordinal:
            out.append(Attr(tid, nid, t.key, t.name_de, t.dimension, "", None, None, "provided", category=v))
        else:
            out.append(Attr(tid, nid, t.key, t.name_de, t.dimension, BASE_UNIT[t.dimension], v[0], v[1], "provided"))
    return out


# --------------------------------------------------------------------------- programmes

ROW_CLASSES = {
    "front": {
        "economy": {"seat_track": 1, "height_adjuster": 1, "recliner": 1, "headrest_manual": 1,
                    "seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "side_airbag": 1,
                    "occupant_detection": 0.6, "seat_heating": 0.5, "armrest": 0.3},
        "mid": {"seat_track": 1, "height_adjuster": 1, "recliner": 1, "lumbar_support": 1,
                "headrest_manual": 0.5, "headrest_powered": 0.5, "seat_frame": 1, "foam_pad": 1,
                "trim_cover": 1, "wiring": 1, "side_airbag": 1, "occupant_detection": 1, "seat_heating": 1,
                "seat_motor": 0.8, "belt_presenter": 0.3, "armrest": 0.5, "seat_ventilation": 0.3},
        "premium": {"seat_track": 1, "height_adjuster": 1, "recliner": 1, "lumbar_support": 1,
                    "headrest_powered": 1, "seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1,
                    "side_airbag": 1, "occupant_detection": 1, "seat_heating": 1, "seat_ventilation": 1,
                    "massage": 0.8, "memory_module": 1, "seat_motor": 1, "side_bolster": 0.7,
                    "cushion_extension": 0.7, "belt_presenter": 0.6, "armrest": 0.6},
    },
    "second": {
        "economy": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "recliner": 0.6,
                    "fold_flat": 1, "headrest_manual": 1, "armrest": 0.4, "easy_entry": 0.3},
        "mid": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "recliner": 1, "fold_flat": 1,
                "headrest_manual": 1, "armrest": 0.8, "easy_entry": 0.5, "seat_heating": 0.6,
                "seat_track": 0.4},
        "premium": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "recliner": 1,
                    "fold_flat": 1, "headrest_powered": 0.6, "headrest_manual": 0.4, "armrest": 1,
                    "easy_entry": 0.7, "seat_heating": 1, "seat_ventilation": 0.5, "seat_track": 0.7,
                    "seat_motor": 0.6, "massage": 0.2},
    },
    "third": {
        "economy": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "fold_flat": 1,
                    "headrest_manual": 1},
        "mid": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "fold_flat": 1,
                "headrest_manual": 1, "recliner": 0.5},
        "premium": {"seat_frame": 1, "foam_pad": 1, "trim_cover": 1, "wiring": 1, "fold_flat": 1,
                    "headrest_manual": 1, "recliner": 1, "seat_heating": 0.5, "seat_motor": 0.4},
    },
}

ROW_LABEL = {"front": "front row", "second": "2nd row", "third": "3rd row"}


@dataclass
class Programme:
    pid: str
    maker: str
    row: str
    trim: str
    year: int
    capability_only: bool
    items: list  # (node_id, Item, label, module_ref)


def build_programmes(seed: int, mod_nodes, apps):
    rng = rng_for(seed, "programmes")
    # application candidates per class: single-class items and combo items
    single = defaultdict(list)  # class -> [(item node id, Item, label)]
    combos = []  # (classes set, [(item node id, Item, label)])
    label_of = {n.node_id: n.label for n in mod_nodes}
    for aid in sorted(apps):
        entries = [(nid, it, label_of[nid]) for nid, it in apps[aid]]
        if len(entries) == 1:
            single[entries[0][1].class_id].append(entries[0])
        else:
            combos.append((frozenset(it.class_id for _, it, _ in entries), entries))
    wordings = defaultdict(list)
    for w in VOCABULARY:
        wordings[w.class_id].append(w.text)

    n_cap = int(round(CAPABILITY_ONLY_SHARE * N_PROGRAMMES))
    cap_idx = set(int(i) for i in rng.choice(N_PROGRAMMES, size=n_cap, replace=False))
    progs = []
    for i in range(N_PROGRAMMES):
        maker = MAKERS[i % len(MAKERS)][0]
        row = pick(rng, ["front"] * 11 + ["second"] * 6 + ["third"] * 3)
        trim = pick(rng, ["economy", "mid", "mid", "premium", "premium"])
        year = int(rng.integers(2012, 2027))
        probs = ROW_CLASSES[row][trim]
        classes = [c for c in sorted(probs) if rng.random() < probs[c]]
        pid = f"P{i + 1:03d}"
        items = []
        cap = i in cap_idx
        k = 0
        if cap:
            for c in classes:
                cls = CLASS_BY_ID[c]
                k += 1
                it = Item(c, sample_count(rng, cls), {t.key: sample_value(rng, t) for t in cls.attrs})
                items.append((f"{pid}.I{k:02d}", it, pick(rng, wordings[c]), ""))
        else:
            remaining = list(classes)
            for cset, entries in combos:
                if cset <= set(remaining) and rng.random() < 0.45:
                    for nid, it, lab in entries:
                        k += 1
                        items.append((f"{pid}.I{k:02d}", it, lab, nid))
                    remaining = [c for c in remaining if c not in cset]
            for c in remaining:
                nid, it, lab = pick(rng, single[c])
                k += 1
                items.append((f"{pid}.I{k:02d}", it, lab, nid))
        items.sort(key=lambda t: t[0])
        progs.append(Programme(pid, maker, row, trim, year, cap, per_side(pid, row, items)))
    return progs


SIDES = {"driver": ("D", "Fahrersitz", "Driver seat"), "passenger": ("B", "Beifahrersitz", "Passenger seat")}
DRIVER_ONLY = {"memory_module"}
PASSENGER_ONLY = {"occupant_detection"}


def per_side(pid: str, row: str, items: list) -> list:
    """Front rows list the driver and the passenger seat separately; other rows are one bench."""
    if row != "front":
        return [(nid, it, lab, ref, "") for nid, it, lab, ref in items]
    out = []
    for side, (code, _, _) in SIDES.items():
        for k, (_, it, lab, ref) in enumerate(items, start=1):
            if (side == "passenger" and it.class_id in DRIVER_ONLY) or \
                    (side == "driver" and it.class_id in PASSENGER_ONLY):
                continue
            out.append((f"{pid}.{code}{k:02d}", it, lab, ref, side))
    return out


def programme_tree(p: Programme):
    nodes, attrs = [], []
    maker_code = dict((m[0], m[2]) for m in MAKERS)[p.maker]
    label = f"{maker_code}-{p.pid[1:]} {ROW_LABEL[p.row]} {p.trim} MY{p.year}"
    nodes.append(Node(p.pid, p.pid, "", "container", "AND", label, source=INTERNAL_SOURCE))
    groups = {}
    for nid, it, lab, ref, side in p.items:
        parent = p.pid
        if side:
            sid = f"{p.pid}.{SIDES[side][0]}"
            if (side, None) not in groups:
                groups[(side, None)] = sid
                nodes.append(Node(p.pid, sid, p.pid, "container", "AND", SIDES[side][1], source=INTERNAL_SOURCE))
            parent = sid
        if not p.capability_only:
            g = CLASS_BY_ID[it.class_id].group
            if (side, g) not in groups:
                gid = f"{parent}.G{sum(1 for k in groups if k[0] == side and k[1]) + 1}"
                groups[(side, g)] = gid
                nodes.append(Node(p.pid, gid, parent, "container", "AND", GROUP_LABELS["de"][g],
                                  source=INTERNAL_SOURCE))
            parent = groups[(side, g)]
        nodes.append(Node(p.pid, nid, parent, "item", "AND", lab, count=it.count, source=INTERNAL_SOURCE,
                          extra={"module_ref": ref, "class_id": it.class_id}))
        attrs.extend(provided_attrs(p.pid, nid, it))
    return nodes, attrs


# --------------------------------------------------------------------------- requests

AT_LEAST = {"en": ["at least {v}", "min. {v}", "≥ {v}", "not less than {v}", "minimum {v}"],
            "de": ["mindestens {v}", "min. {v}", "≥ {v}", "nicht weniger als {v}", "minimal {v}"]}
AT_MOST = {"en": ["max. {v}", "at most {v}", "≤ {v}", "up to {v}", "not more than {v}"],
           "de": ["max. {v}", "höchstens {v}", "≤ {v}", "bis zu {v}", "maximal {v}"]}
WITHIN = {"en": ["{a}–{b} {u}", "{a} to {b} {u}", "between {a} and {b} {u}"],
          "de": ["{a}–{b} {u}", "{a} bis {b} {u}", "zwischen {a} und {b} {u}"]}
ORDINAL_TXT = {"en": ["{c} or better", "at least {c}", "min. {c}"],
               "de": ["{c} oder besser", "mindestens {c}", "min. {c}"]}
WITH = {"en": " with ", "de": " mit "}
AND = {"en": " and ", "de": " und "}
NOTES = {"en": ["Note: all values apply at 23 °C ambient unless stated otherwise.",
                "Note: the supplier shall confirm each requirement in the compliance matrix.",
                "Note: where no seat is named, the requirement applies to both front seats."],
         "de": ["Hinweis: Alle Werte gelten bei 23 °C Umgebung, sofern nicht anders angegeben.",
                "Hinweis: Der Lieferant bestätigt jede Anforderung in der Konformitätsmatrix.",
                "Hinweis: Ohne Sitzangabe gilt die Anforderung für beide Vordersitze."]}
TITLES = {"en": "Seat system specification", "de": "Lastenheft Sitzanlage"}
SUBSECTION = {"en": "Performance", "de": "Leistungsdaten"}


@dataclass
class ReqAttr:
    attr_id: str
    tmpl: AttrTemplate
    name: str
    text: str
    bound: str
    lo: float | None
    hi: float | None
    category: str
    stretched: bool
    src: tuple  # source (lo, hi) or category
    value_unit: str = ""
    value_lo: float | None = None
    value_hi: float | None = None
    qualified: bool = True


@dataclass
class ReqItem:
    node_id: str
    class_id: str
    count: float
    phrasing_id: str
    phrasing: str
    attrs: list
    source_item: Item
    stretch_level: int = 0  # 0 none, 1 beyond source, 2 beyond any module
    side: str = ""


def phrasing_ids():
    out = {}
    for cid in sorted(PHRASINGS):
        for split in ("dev", "test"):
            for i, p in enumerate(PHRASINGS[cid][split], start=1):
                out[(cid, split, i)] = (f"{cid}.{split}{i}", p)
    return out


def render_value(rng, tmpl: AttrTemplate, lang: str, bound: str, lo: float, hi: float, qualified: bool,
                 unit: str):
    """Render a requirement (base units) in a customer unit. Returns text and the customer values."""
    thousands = tmpl.dimension == "count" and rng.random() < 0.5

    def num(x):
        return render_number(x, lang, thousands)

    u = f" {unit}" if unit else ""
    if bound == "within":
        a, b = from_base(lo, unit), from_base(hi, unit)
        return pick(rng, WITHIN[lang]).format(a=num(a), b=num(b), u=unit).rstrip(), a, b
    v = from_base(lo if bound in ("at_least", "equals") else hi, unit)
    text = f"{num(v)}{u}"
    if qualified and bound == "at_least":
        text = pick(rng, AT_LEAST[lang]).format(v=text)
    elif qualified and bound == "at_most":
        text = pick(rng, AT_MOST[lang]).format(v=text)
    return text, v, v


def derive_value(rng, tmpl: AttrTemplate, src, level: int, qualified: bool, unit: str):
    """Required (lo, hi) in base units, derived from the source value.

    level 0: satisfied by the source; 1: beyond the source; 2: beyond the whole value range.
    Values are rounded in the customer unit in the direction that keeps level 0
    satisfied and levels 1/2 unsatisfied.
    """
    step = unit_step(unit, tmpl)
    p_lo, p_hi = src
    nat_lo, nat_hi = to_base(tmpl.lo, tmpl.natural_unit), to_base(tmpl.hi, tmpl.natural_unit)
    if tmpl.dimension == "temperature":
        # compare in °C distances, not Kelvin ratios
        def scale(x, f):
            return to_base(from_base(x, "°C") * f, "°C")
    else:
        def scale(x, f):
            return x * f

    def cu(x, mode, fine=False):
        c = from_base(x, unit)
        c = sig_round(c, 3, mode) if fine else round_to(c, step, mode)
        return to_base(c, unit)

    b = tmpl.bound
    if b == "at_least":
        if level == 0:
            v = cu(p_hi, "down", fine=True) if not qualified else cu(scale(p_hi, rng.uniform(0.85, 0.98)), "down")
            if v <= 0:
                v = cu(p_hi, "down", fine=True)
        elif level == 1:
            v = cu(scale(p_hi, rng.uniform(1.15, 1.3)), "up")
        else:
            v = cu(scale(nat_hi, rng.uniform(1.3, 1.5)), "up")
        return v, v
    if b == "at_most":
        if level == 0:
            v = cu(p_lo, "up", fine=True) if not qualified else cu(scale(p_lo, rng.uniform(1.02, 1.15)), "up")
        elif level == 1:
            v = cu(scale(p_lo, 1 / rng.uniform(1.15, 1.3)), "down")
        else:
            v = cu(scale(nat_lo, rng.uniform(0.55, 0.7)), "down")
        return v, v
    if b == "within":
        span = p_hi - p_lo
        r_lo = cu(p_lo + rng.uniform(0.05, 0.3) * span, "up")
        if level == 0:
            r_hi = cu(p_hi - rng.uniform(0.05, 0.3) * span, "down")
        elif level == 1:
            r_hi = cu(p_hi * rng.uniform(1.15, 1.3), "up")
        else:
            r_hi = cu(nat_hi * rng.uniform(1.3, 1.5), "up")
        if r_hi <= r_lo:
            r_hi = cu(r_lo + step * UNITS[unit][1], "up") if level else r_lo
        return r_lo, r_hi
    if b == "equals":
        if level == 0:
            v = cu(p_lo, "near", fine=True)
        elif level == 1:
            v = cu(p_lo * rng.uniform(1.08, 1.15), "up")
        else:
            v = cu(nat_hi * rng.uniform(1.3, 1.5), "up")
        return v, v
    raise ValueError(b)


def build_requests(seed: int, progs: list[Programme], modules_items):
    rng = rng_for(seed, "requests")
    pids = phrasing_ids()
    # sources: bias towards front-row programmes, which carry more items
    weights = np.array([1.6 if p.row == "front" else 1.0 for p in progs])
    src_idx = rng.choice(len(progs), size=N_REQUESTS, replace=False, p=weights / weights.sum())
    unmeetable = set(int(i) for i in rng.choice(N_REQUESTS, size=N_UNMEETABLE, replace=False))
    requests = []
    for r, pi in enumerate(sorted(int(i) for i in src_idx)):
        p = progs[pi]
        rid = f"R{r + 1:02d}"
        lang = "de" if r % 2 == 0 else "en"
        granularity = "coarse" if rng.random() < 0.3 else "detailed"
        items = []
        for j, (_, it, _, _, side) in enumerate(p.items, start=1):
            if rng.random() < 0.04:
                continue
            cls = CLASS_BY_ID[it.class_id]
            cands = [(k, ph) for (cid, split, k), (_, ph) in sorted(pids.items())
                     if cid == it.class_id and split == "dev"]
            same_lang = [(k, ph) for k, ph in cands if ph.lang == lang] or cands
            k, ph = pick(rng, same_lang)
            phid = pids[(it.class_id, "dev", k)][0]
            items.append(ReqItem(f"{rid}.r{j:02d}", it.class_id, it.count, phid, ph.text, [], it, side=side))
        # which attributes the request states
        for ri in items:
            cls = CLASS_BY_ID[ri.class_id]
            p_state = 0.8 if granularity == "detailed" else 0.12
            ri.attrs = [t for t in cls.attrs if rng.random() < p_state]
        # stretch plan
        if r in unmeetable:
            numeric_items = [ri for ri in items if any(not t.is_ordinal for t in CLASS_BY_ID[ri.class_id].attrs)]
            chosen = rng.choice(len(numeric_items), size=min(len(numeric_items), int(rng.integers(7, 10))),
                                replace=False)
            for c in sorted(int(x) for x in chosen):
                ri = numeric_items[c]
                ri.stretch_level = 2
                numeric = [t for t in CLASS_BY_ID[ri.class_id].attrs if not t.is_ordinal]
                if not any(not t.is_ordinal for t in ri.attrs):
                    ri.attrs = ri.attrs + [numeric[0]]
        else:
            with_attrs = [ri for ri in items if len(ri.attrs) >= 3] or [ri for ri in items if ri.attrs]
            n = min(len(with_attrs), int(rng.choice([1, 2, 3, 3, 4])))
            for c in sorted(int(x) for x in rng.choice(len(with_attrs), size=n, replace=False)):
                with_attrs[c].stretch_level = 1
        requests.append({"rid": rid, "programme": p, "lang": lang, "granularity": granularity,
                         "items": items, "unmeetable_plan": r in unmeetable})
    # materialise values
    for req in requests:
        materialise(rng, req)
    return requests


def choose_unit(rng, tmpl: AttrTemplate, lang: str) -> str:
    units = list(tmpl.customer_units) or [""]
    if lang == "de":
        units = [u for u in units if u not in ("cycles",)] or units
    else:
        units = [u for u in units if u not in ("Zyklen",)] or units
    return pick(rng, units)


def materialise(rng, req):
    lang = req["lang"]
    for ri in req["items"]:
        out = []
        cls = CLASS_BY_ID[ri.class_id]
        order = {t.key: i for i, t in enumerate(cls.attrs)}
        ri.attrs = sorted(ri.attrs, key=lambda t: order[t.key])
        # level 1 stretches one attribute; level 2 stretches every numeric attribute
        level1_key = None
        if ri.stretch_level == 1:
            stretchable = [t for t in ri.attrs if not t.is_ordinal or
                           ORDINALS[t.dimension].index(ri.source_item.values[t.key]) < len(ORDINALS[t.dimension]) - 1]
            level1_key = pick(rng, stretchable).key if stretchable else None
            if level1_key is None:
                ri.stretch_level = 0
        for t in ri.attrs:
            src = ri.source_item.values[t.key]
            name = pick(rng, t.names_de if lang == "de" else t.names_en)
            level = 2 if (ri.stretch_level == 2 and not t.is_ordinal) else (1 if t.key == level1_key else 0)
            if t.is_ordinal:
                scale = ORDINALS[t.dimension]
                si = scale.index(src)
                cat = scale[si + 1] if level else scale[max(0, si - int(rng.random() < 0.4))]
                text = pick(rng, ORDINAL_TXT[lang]).format(c=cat)
                out.append(ReqAttr(f"a{len(out) + 1}", t, name, text, t.bound, None, None, cat, level > 0, src))
                continue
            qualified = True
            if t.bound in ("at_least", "at_most"):
                qualified = rng.random() >= UNQUALIFIED_SHARE
            elif t.bound == "equals":
                qualified = False
            unit = choose_unit(rng, t, lang)
            lo, hi = derive_value(rng, t, src, level, qualified, unit)
            text, vlo, vhi = render_value(rng, t, lang, t.bound, lo, hi, qualified, unit)
            out.append(ReqAttr(f"a{len(out) + 1}", t, name, text, t.bound, lo, hi, "", level > 0, src,
                               unit, vlo, vhi, qualified))
        ri.attrs = out


def item_label(maker: str, lang: str, ri: ReqItem) -> str:
    x = ri.phrasing
    if ri.count > 1:
        x = f"{int(ri.count)} × {x}"
    if ri.attrs:
        x += WITH[lang] + AND[lang].join(f"{a.name} {a.text}" for a in ri.attrs)
    sentence = FRAMES[maker][lang].format(x=x)
    if ri.side:
        sentence = f"{SIDES[ri.side][1 if lang == 'de' else 2]}: {sentence}"
    return sentence


def request_tree(rng, req):
    rid, lang, p = req["rid"], req["lang"], req["programme"]
    maker_code = dict((m[0], m[2]) for m in MAKERS)[p.maker]
    nodes = [Node(rid, rid, "", "container", "AND", f"{TITLES[lang]} {maker_code}-{rid}", source=p.maker)]
    nodes.append(Node(rid, f"{rid}.n0", rid, "container", "AND", pick(rng, NOTES[lang]), source=p.maker))
    by_group = defaultdict(list)
    for ri in req["items"]:
        by_group[CLASS_BY_ID[ri.class_id].group].append(ri)
    attrs = []
    for ci, g in enumerate(["structure", "comfort", "safety", "electrics"], start=1):
        if not by_group[g]:
            continue
        chap = f"{rid}.c{ci}"
        chap_label = f"{ci} {GROUP_LABELS[lang][g]}"
        nodes.append(Node(rid, chap, rid, "container", "AND", chap_label, source=p.maker))
        by_class = defaultdict(list)
        for ri in by_group[g]:
            by_class[ri.class_id].append(ri)
        for si, cid in enumerate(sorted(by_class, key=lambda c: min(r.node_id for r in by_class[c])), start=1):
            cls = CLASS_BY_ID[cid]
            layout = rng.random()
            if layout < 0.15:  # requirements directly under the chapter
                parent, context = chap, chap_label
            else:
                sec = f"{chap}.s{si}"
                heading = f"{ci}.{si} {cls.label_de if lang == 'de' else cls.label_en}"
                nodes.append(Node(rid, sec, chap, "container", "AND", heading, source=p.maker))
                parent, context = sec, heading
                if rng.random() < 0.1:
                    nodes.append(Node(rid, f"{sec}.n1", sec, "container", "AND", pick(rng, NOTES[lang]),
                                      source=p.maker))
                if layout > 0.8:  # one level deeper
                    sub = f"{sec}.1"
                    sub_label = f"{ci}.{si}.1 {SUBSECTION[lang]}"
                    nodes.append(Node(rid, sub, sec, "container", "AND", sub_label, source=p.maker))
                    parent, context = sub, sub_label
            for ri in by_class[cid]:
                nodes.append(Node(rid, ri.node_id, parent, "item", "AND", item_label(p.maker, lang, ri),
                                  context=context, count=ri.count, source=p.maker,
                                  extra={"class_id": ri.class_id, "phrasing_id": ri.phrasing_id}))
                for at in ri.attrs:
                    attrs.append((ri.node_id, at))
    return nodes, attrs


# --------------------------------------------------------------------------- oracle


def oracle_item_score(ri: ReqItem, cands: list[Item]) -> float:
    if not cands:
        return 0.0

    def a_of(c: Item) -> float:
        if not ri.attrs:
            return 1.0
        s = 0
        for x in ri.attrs:
            v = c.values.get(x.tmpl.key)
            if x.category:
                s += ordinal_satisfies(x.tmpl.dimension, x.category, v)
            else:
                s += oracle_satisfies(x.bound, x.lo, x.hi, v[0], v[1])
        return s / len(ri.attrs)

    a = max(a_of(c) for c in cands)
    return a * min(1.0, sum(c.count for c in cands) / ri.count)


def oracle_coverage(req, prog: Programme) -> float:
    by_class = defaultdict(list)
    for _, it, _, _, _ in prog.items:
        by_class[it.class_id].append(it)
    scores = [oracle_item_score(ri, by_class.get(ri.class_id, [])) for ri in req["items"]]
    return sum(scores) / len(scores)


def calibrate_requests(seed, requests, progs):
    """Make every request land clearly on its side of the 0.95 flag threshold."""
    rng = rng_for(seed, "calibrate")
    for req in requests:
        for _ in range(20):
            best = max(oracle_coverage(req, p) for p in progs)
            if req["unmeetable_plan"] and best >= 0.95:
                cand = [ri for ri in req["items"] if ri.stretch_level < 2 and
                        any(not t.is_ordinal for t in CLASS_BY_ID[ri.class_id].attrs)]
                ri = pick(rng, cand)
                ri.stretch_level = 2
                cls_attrs = [t for t in CLASS_BY_ID[ri.class_id].attrs if not t.is_ordinal]
                keep = {a.tmpl.key for a in ri.attrs}
                ri.attrs = [t for t in CLASS_BY_ID[ri.class_id].attrs if t.key in keep or t.key == cls_attrs[0].key]
                rematerialise(rng, req, ri)
            elif not req["unmeetable_plan"] and best < 0.95:
                stretched = [ri for ri in req["items"] if ri.stretch_level]
                ri = stretched[-1]
                ri.stretch_level = 0
                ri.attrs = [a.tmpl for a in ri.attrs]
                rematerialise(rng, req, ri)
            else:
                break


def rematerialise(rng, req, ri):
    ri.attrs = [a.tmpl if isinstance(a, ReqAttr) else a for a in ri.attrs]
    sub = dict(req, items=[ri])
    materialise(rng, sub)


# --------------------------------------------------------------------------- output


def write_csv(out: Path, name: str, header: list[str], rows) -> None:
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    w.writerow(header)
    for r in rows:
        w.writerow(["" if v is None else v for v in r])
    raw = buf.getvalue().encode("utf-8")
    with open(out / f"{name}.csv.gz", "wb") as fh:
        with gzip.GzipFile(filename="", mode="wb", fileobj=fh, mtime=0, compresslevel=9) as gz:
            gz.write(raw)


NODE_HEADER = ["tree_id", "node_id", "parent_id", "node_kind", "edge_type", "label", "context", "class_id",
               "count", "source"]
ATTR_HEADER = ["tree_id", "node_id", "attr_id", "name", "dimension", "unit", "lo", "hi", "role", "bound",
               "qualifier", "category"]


def node_row(n: Node, *extra_cols):
    return [n.tree_id, n.node_id, n.parent_id, n.node_kind, n.edge_type, n.label, n.context, "",
            fmt_num(n.count), n.source] + [n.extra.get(c, "") for c in extra_cols]


def attr_row(a: Attr):
    return [a.tree_id, a.node_id, a.attr_id, a.name, a.dimension, a.unit, fmt_num(a.lo), fmt_num(a.hi), a.role,
            a.bound, "", a.category]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args(argv)
    seed, out = args.seed, args.output
    out.mkdir(parents=True, exist_ok=True)
    (out / "requests_json").mkdir(exist_ok=True)
    for old in (out / "requests_json").glob("*.json"):
        old.unlink()

    # catalogue
    write_csv(out, "catalogue_classes", ["class_id", "label_en", "label_de", "definition"],
              [[c.class_id, c.label_en, c.label_de, c.definition] for c in CLASSES])
    write_csv(out, "catalogue_attributes",
              ["class_id", "attr_key", "name_en", "name_de", "dimension", "base_unit", "bound", "scale"],
              [[c.class_id, t.key, t.name_en, t.name_de, t.dimension, BASE_UNIT.get(t.dimension, ""),
                t.bound, t.scale] for c in CLASSES for t in c.attrs])
    rules = []
    for dim, scale in ORDINALS.items():
        for i, req in enumerate(scale):
            for j, prov in enumerate(scale):
                rules.append([dim, req, prov, fmt_num(1.0 if j >= i else (0.5 if j == i - 1 else 0.0))])
    write_csv(out, "compatibility_rules", ["dimension", "required", "provided", "score"], rules)
    write_csv(out, "internal_vocabulary", ["wording", "source"],
              [[w.text, INTERNAL_SOURCE] for w in VOCABULARY])
    write_csv(out, "gold_vocabulary", ["wording", "source", "class_id", "apparent_class_id", "is_misleading"],
              [[w.text, INTERNAL_SOURCE, w.class_id, w.apparent_class_id or w.class_id,
                "true" if w.misleading else "false"] for w in VOCABULARY])

    # module library
    mod_nodes, mod_attrs, apps = build_modules(seed)
    write_csv(out, "module_nodes", NODE_HEADER + ["application_id"],
              [node_row(n, "application_id") for n in mod_nodes])
    write_csv(out, "module_attributes", ATTR_HEADER, [attr_row(a) for a in mod_attrs])

    # programmes
    progs = build_programmes(seed, mod_nodes, apps)
    p_nodes, p_attrs = [], []
    for p in progs:
        n, a = programme_tree(p)
        p_nodes += n
        p_attrs += a
    write_csv(out, "programme_nodes", NODE_HEADER + ["module_ref"], [node_row(n, "module_ref") for n in p_nodes])
    write_csv(out, "programme_attributes", ATTR_HEADER, [attr_row(a) for a in p_attrs])
    apparent = {w.text: (w.apparent_class_id or w.class_id) for w in VOCABULARY}
    reviewed_ms, unreviewed_ms = defaultdict(float), defaultdict(float)
    for n in p_nodes:
        if n.node_kind == "item":
            reviewed_ms[(n.tree_id, n.extra["class_id"])] += n.count
            unreviewed_ms[(n.tree_id, apparent[n.label])] += n.count
    for name, ms in (("programme_class_multiset", reviewed_ms), ("programme_class_multiset_unreviewed",
                                                                 unreviewed_ms)):
        write_csv(out, name, ["entity_id", "feature_id", "weight"],
                  [[e, f, fmt_num(w)] for (e, f), w in sorted(ms.items())])

    # requests
    requests = build_requests(seed, progs, apps)
    calibrate_requests(seed, requests, progs)
    tree_rng = rng_for(seed, "request_trees")
    r_nodes, r_attrs = [], []
    for req in requests:
        n, a = request_tree(tree_rng, req)
        req["nodes"], req["attrs"] = n, a
        r_nodes += n
        r_attrs += [(req["rid"], nid, at) for nid, at in a]
    write_csv(out, "request_nodes", NODE_HEADER, [node_row(n) for n in r_nodes])
    write_csv(out, "request_attributes", ["tree_id", "node_id", "attr_id", "name", "value_text", "role"],
              [[rid, nid, a.attr_id, a.name, a.text, "required"] for rid, nid, a in r_attrs])
    write_csv(out, "gold_request_attributes",
              ["tree_id", "node_id", "attr_id", "attr_key", "dimension", "unit", "lo", "hi", "bound", "category",
               "stretched", "src_lo", "src_hi", "src_category"],
              [[rid, nid, a.attr_id, a.tmpl.key, a.tmpl.dimension,
                "" if a.category else BASE_UNIT[a.tmpl.dimension], fmt_num(a.lo), fmt_num(a.hi), a.bound,
                a.category, "true" if a.stretched else "false",
                "" if a.category else fmt_num(a.src[0]), "" if a.category else fmt_num(a.src[1]),
                a.src if a.category else ""] for rid, nid, a in r_attrs])
    for req in requests:
        write_request_json(out, req)

    # gold labels for every item
    gold_rows = [[n.tree_id, n.node_id, n.extra["class_id"], n.extra.get("phrasing_id", "")]
                 for n in mod_nodes + p_nodes + r_nodes if n.node_kind == "item"]
    write_csv(out, "gold_item_class", ["tree_id", "node_id", "class_id", "phrasing_id"], gold_rows)

    # gold coverage, requests, best applications
    cov_rows, req_rows = [], []
    for req in requests:
        covs = [(oracle_coverage(req, p), p.pid) for p in progs]
        best_cov = max(c for c, _ in covs)
        best_pid = min(pid for c, pid in covs if c == best_cov)
        cov_rows += [[req["rid"], pid, fmt_num(c)] for c, pid in covs]
        req_rows.append([req["rid"], req["programme"].pid, req["programme"].maker, req["lang"],
                         req["granularity"], "true" if best_cov < 0.95 else "false", best_pid, fmt_num(best_cov)])
    write_csv(out, "gold_coverage", ["query_id", "candidate_id", "true_coverage"], cov_rows)
    write_csv(out, "gold_requests", ["request_id", "source_programme_id", "maker", "lang", "granularity",
                                     "is_unmeetable", "best_programme_id", "best_coverage"], req_rows)
    best_rows = []
    for req in requests:
        for ri in req["items"]:
            scored = []
            for aid in sorted(apps):
                for _, it in apps[aid]:
                    if it.class_id == ri.class_id:
                        s = oracle_item_score(dataclass_replace_count(ri), [it])
                        scored.append((aid, s))
            top = max((s for _, s in scored), default=0.0)
            best_rows += [[req["rid"], ri.node_id, aid, fmt_num(s), "true" if s == top and top > 0 else "false"]
                          for aid, s in scored]
    write_csv(out, "gold_best_applications", ["tree_id", "node_id", "application_id", "oracle_score", "is_best"],
              best_rows)

    # phrasing pool and unseen requirements
    pids = phrasing_ids()
    write_csv(out, "phrasings", ["phrasing_id", "class_id", "lang", "split", "text"],
              [[pid, cid, ph.lang, split, ph.text] for (cid, split, _), (pid, ph) in sorted(pids.items())])
    unseen = build_unseen(seed, pids)
    write_csv(out, "unseen_requirements", ["item_id", "source", "lang", "text"],
              [[u["item_id"], u["source"], u["lang"], u["text"]] for u in unseen])
    write_csv(out, "gold_unseen", ["item_id", "class_id", "phrasing_id"],
              [[u["item_id"], u["class_id"], u["phrasing_id"]] for u in unseen])

    # vectors
    vectors_mod.write_all(out, seed, write_csv, requests, unseen, pids)

    # customer master
    cust, gold_clusters = customers_mod.build(seed)
    write_csv(out, "customers", customers_mod.HEADER, cust)
    write_csv(out, "gold_customer_clusters", ["record_id", "cluster_id", "dup_kind", "lookalike_of"],
              gold_clusters)


def dataclass_replace_count(ri: ReqItem) -> ReqItem:
    """Best application per requirement ignores the count (instances needed)."""
    return ReqItem(ri.node_id, ri.class_id, 1.0, ri.phrasing_id, ri.phrasing, ri.attrs, ri.source_item, side=ri.side)


def build_unseen(seed, pids):
    rng = rng_for(seed, "unseen")
    out = []
    keys = sorted(k for k in pids if k[1] == "test")
    for i in range(N_UNSEEN):
        cid, _, k = pick(rng, keys)
        pid, ph = pids[(cid, "test", k)]
        maker = MAKERS[int(rng.integers(len(MAKERS)))][0]
        out.append({"item_id": f"U{i + 1:03d}", "class_id": cid, "phrasing_id": pid, "lang": ph.lang,
                    "source": maker, "text": FRAMES[maker][ph.lang].format(x=ph.text), "phrasing": ph})
    return out


def write_request_json(out: Path, req) -> None:
    attrs_by_node = defaultdict(list)
    for nid, a in req["attrs"]:
        attrs_by_node[nid].append(a)
    children = defaultdict(list)
    for n in req["nodes"]:
        children[n.parent_id].append(n)

    def enc(n: Node) -> dict:
        d = {"node_id": n.node_id, "kind": n.node_kind, "edge": n.edge_type, "label": n.label}
        if n.context:
            d["context"] = n.context
        if n.node_kind == "item":
            d["count"] = n.count
        if attrs_by_node.get(n.node_id):
            d["attributes"] = [attr_json(a) for a in attrs_by_node[n.node_id]]
        if children.get(n.node_id):
            d["children"] = [enc(c) for c in children[n.node_id]]
        return d

    root = children[""][0]
    doc = {"tree_id": req["rid"], "source": req["programme"].maker, "lang": req["lang"], "root": enc(root)}
    (out / "requests_json" / f"{req['rid']}.json").write_text(
        json.dumps(doc, ensure_ascii=False, indent=1, sort_keys=True) + "\n", encoding="utf-8")


def attr_json(a: ReqAttr) -> dict:
    """What upstream extraction delivers: name, text and separate value fields (BRD A1)."""
    d = {"attr_id": a.attr_id, "name": a.name, "text": a.text}
    if a.category:
        d.update(category=a.category, bound=a.bound)
    else:
        d.update(lo=round(a.value_lo, 6), hi=round(a.value_hi, 6), unit=a.value_unit,
                 bound=(a.bound if a.qualified else None))
    return d


if __name__ == "__main__":
    main()

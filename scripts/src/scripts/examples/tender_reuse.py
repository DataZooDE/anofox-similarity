"""Deterministic generator for examples/tender_reuse/ (round-4 scenario folder).

Story: Allgäuer Abfülltechnik Demo GmbH (fictional) answers RFQ-2026-014 of Molkerei Bergblick
Demo eG (fictional) for a yoghurt-cup filling and sealing line. The bid engineer checks 42 clauses
(37 must, 5 should) against 12 past lines and 10 module variants.

    cd scripts && uv run python -m scripts.examples.tender_reuse

Writes examples/tender_reuse/data/: the extracted request (rfq_2026_014.json), the clause sidecar
(clauses.csv), the past lines and modules as node/attribute tables, the catalogue templates, the
house vocabulary, the class rules and the ground truth (gold_deviations.csv,
gold_station_cover.csv). Values of past lines
and modules are in base units (m, W, Pa, K, m3/s, s), as a PLM export would give them, except for
the planted dirty-unit row. Everything is fictional; there is no randomness except a seeded choice
of filler items.
"""

import csv
import json
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / "examples" / "tender_reuse" / "data"
RFQ_ID = "RFQ-2026-014"
SEED = 2026014

# ---------------------------------------------------------------------------------------------
# The request: (station, item, class_id or None, label, count, must_should, page, attributes,
# manual clauses). Attributes: (attr_id, name, text, must_should or None = item's, page or None).
# Manual clauses are designations and standards the kernel cannot check (see the parse audit).
# ---------------------------------------------------------------------------------------------
STATIONS = {
    "D": "Dosierstation",
    "S": "Siegelstation",
    "F": "Becherzufuhr und Förderer",
    "C": "Reinigung (CIP)",
    "L": "Linie, Steuerung, Etikettierung",
}
RFQ = [
    ("D", "D1", "dosing_zone", "Dosierzone Joghurt", 4, "must", 14,
     [("stroke", "Dosierhub", "mind 60 mm"),                      # trap a: 'mind' without period
      ("volume", "Dosiervolumen", "mind. 150 ml"),
      ("accuracy", "Dosiergenauigkeit", "± 0,5 g")],              # trap: bare ± is at_most
     [("certificate", "Abnahmeprüfzeugnis EN 10204 3.1")]),
    ("D", "D2", "dosing_drive", "Dosierantrieb Hauptdosierung", 1, "must", 15,
     [("power", "Antriebsleistung", "mind. 90 W"),                # trap k (with D3)
      ("torque", "Drehmoment", "mind. 1,2 Nm")], []),
    ("D", "D3", "dosing_drive", "Dosierantrieb Fruchtdosierung", 1, "must", 15,
     [("power", "Antriebsleistung", "mind. 10 W"),
      ("torque", "Drehmoment", "mind. 0,2 Nm")], []),
    ("S", "S1", "sealing_unit", "Siegelstation", 1, "must", 22,
     [("temperature", "Siegeltemperatur", "mindestens 200 °C"),
      ("current", "Stromaufnahme Heizung", "höchstens 7,5 A"),    # trap c: decimal comma
      ("maintenance", "Wartungsintervall", "mind. 2.500 h"),      # trap d: 2.500 is 2500 in German
      ("noise", "Schalldruckpegel", "72 dB(A)")],                 # trap b: no bound word
     [("material", "Werkstoff produktberührt 1.4404")]),
    ("F", "F1", "conveyor", "Becherförderer", 1, "must", 27,
     [("speed", "Bandgeschwindigkeit", "mindestens 0,5 m/s"),
      ("width", "Bandbreite", "mind. 300 mm"),
      ("noise", "Schalldruckpegel", "nicht über 75 dB(A)")],      # trap j: negated limit
     [("fasteners", "Verbindungselemente M8x1,25 aus A4-70")]),
    ("F", "F2", "cup_denester", "Becherentstapler", 2, "must", 28,
     [("stack_height", "Stapelhöhe", "mind. 600 mm")], []),
    ("C", "C1", None, "Kompaktaggregat Hygiene", 1, "must", 33,  # trap e: class from vocabulary
     [("flow", "Volumenstrom", "mind. 120 l/min"),
      ("temperature", "Reinigungstemperatur", "mind. 85 °C"),
      ("pressure", "Betriebsdruck", "not to exceed 5 bar"),       # EN annex wording
      ("pressure_class", "Druckstufe", "PN 16")],
     [("hygienic_design", "Hygienic design to EN 1672-2")]),
    ("L", "L1", "control_cabinet", "Schaltschrank", 1, "must", 41,
     [("protection", "Schutzart", "IP65 oder besser"),
      ("voltage", "Anschlussspannung", "400 V")], []),           # trap b: no bound word
    ("L", "L2", "line_frame", "Maschinengestell", 1, "must", 42,
     [("length", "Aufstelllänge", "max. 12 m"),
      ("cycle_time", "Taktzeit", "max. 1,2 s"),
      ("output", "Ausbringung", "mind. 1.400 Becher/min", "should", 61)], []),  # trap h
    ("L", "L3", "label_applicator", "Labelling unit (EN annex)", 1, "should", 58,
     [("speed", "label speed", "at least 0.8 m/s"),
      ("protection", "protection class", "IP54 or better")], []),
    ("L", "L4", "operator_panel", "Bedienpanel", 1, "must", 43,
     [("screen", "Bildschirmdiagonale", "mind. 15 Zoll", "should", None)], []),
]

# Base-unit dimension and unit per attribute key (for the past-line export and the catalogue)
DIM = {
    "stroke": ("length", "m"), "volume": ("volume", "m3"), "accuracy": ("mass", "kg"),
    "power": ("power", "W"), "torque": ("torque", "N*m"), "temperature": ("temperature", "K"),
    "current": ("current", "A"), "maintenance": ("time", "s"), "noise": ("sound_level", "dB(A)"),
    "speed": ("speed", "m/s"), "width": ("length", "m"), "stack_height": ("length", "m"),
    "flow": ("volume_flow", "m3/s"), "pressure": ("pressure", "Pa"),
    "pressure_class": ("pressure_class", ""), "protection": ("protection_class", ""),
    "voltage": ("voltage", "V"), "length": ("length", "m"), "cycle_time": ("time", "s"),
    "output": ("frequency", "Hz"), "screen": ("length", "m"),
}
CATEGORY_KEYS = {"pressure_class", "protection"}

# Strict meaning of each requirement in base units: (bound, value); categories: (bound, code)
REQ = {
    ("D1", "stroke"): ("at_least", 0.06), ("D1", "volume"): ("at_least", 0.00015),
    ("D1", "accuracy"): ("at_most", 0.0005),
    ("D2", "power"): ("at_least", 90.0), ("D2", "torque"): ("at_least", 1.2),
    ("D3", "power"): ("at_least", 10.0), ("D3", "torque"): ("at_least", 0.2),
    ("S1", "temperature"): ("at_least", 473.15), ("S1", "current"): ("at_most", 7.5),
    ("S1", "maintenance"): ("at_least", 2500 * 3600.0), ("S1", "noise"): ("at_most", 72.0),
    ("F1", "speed"): ("at_least", 0.5), ("F1", "width"): ("at_least", 0.3),
    ("F1", "noise"): ("at_most", 75.0), ("F2", "stack_height"): ("at_least", 0.6),
    ("C1", "flow"): ("at_least", 0.002), ("C1", "temperature"): ("at_least", 358.15),
    ("C1", "pressure"): ("at_most", 500000.0), ("C1", "pressure_class"): ("at_least", "PN16"),
    ("L1", "protection"): ("at_least", "IP65"), ("L1", "voltage"): ("equals", 400.0),
    ("L2", "length"): ("at_most", 12.0), ("L2", "cycle_time"): ("at_most", 1.2),
    ("L2", "output"): ("at_least", 1400 / 60), ("L3", "speed"): ("at_least", 0.8),
    ("L3", "protection"): ("at_least", "IP54"), ("L4", "screen"): ("at_least", 15 * 0.0254),
}
LADDERS = {"protection_class": ["IP40", "IP54", "IP65", "IP67", "IP69K"],
           "pressure_class": ["PN6", "PN10", "PN16", "PN25", "PN40"]}

# A past line that meets every must clause; output (should) is never reached: trap h.
BASE = {
    "D1": {"count": 4, "stroke": 0.07, "volume": 0.0002, "accuracy": 0.0003},
    "D2": {"count": 1, "power": 120.0, "torque": 1.5},
    "D3": {"count": 1, "power": 25.0, "torque": 0.3},
    "S1": {"count": 1, "temperature": 503.15, "current": 6.5, "maintenance": 3000 * 3600.0,
           "noise": 70.0},
    "F1": {"count": 1, "speed": 0.6, "width": 0.35, "noise": 72.0},
    "F2": {"count": 2, "stack_height": 0.7},
    "C1": {"count": 1, "flow": 0.0025, "temperature": 363.15, "pressure": 400000.0,
           "pressure_class": "PN16"},
    "L1": {"count": 1, "protection": "IP65", "voltage": 400.0},
    "L2": {"count": 1, "length": 10.5, "cycle_time": 1.0, "output": 1200 / 60},
    "L3": {"count": 1, "speed": 1.0, "protection": "IP54"},
    "L4": {"count": 1, "screen": 17 * 0.0254},
}
CLASS_OF = {it[1]: (it[2] or "cip_unit") for it in RFQ}
HOUSE_LABEL = {
    "D1": "Dosierkolben", "D2": "Servoantrieb Dosierung", "D3": "Servoantrieb Dosierung",
    "S1": "Siegelkopf", "F1": "Becherförderer", "F2": "Entstapler", "C1": "CIP-Anlage",
    "L1": "Schaltschrank", "L2": "Grundgestell", "L3": "Etikettierer", "L4": "HMI-Panel",
}

# The 12 past lines: changes against BASE, the item order of the export, fillers, name language.
PAST = {
    "PAST-LEAN-2019": dict(changes={}, fillers=0),                       # trap i: lean full cover
    "PAST-RICH-2022": dict(changes={"L1": {"protection": "IP67"}, "C1": {"pressure_class": "PN25"}},
                           fillers=16),                  # trap i: rich full cover, higher classes
    "PAST-SWAP-2018": dict(changes={"D2": {"power": 20.0, "torque": 0.25},   # trap k: 20 W first
                                    "D3": {"power": 95.0, "torque": 1.3}},
                           fillers=6, english=True),
    "PAST-CUSTOM-2021": dict(changes={"D1": {"count": 2}, "L2": {"output": 1000 / 60}},
                             fillers=8),                                   # trap g: 2 of 4 zones
    "PAST-NOISY-2020": dict(changes={"F1": {"noise": 82.0}}, fillers=5),   # trap j
    "PAST-TOL-2023": dict(changes={"D2": {"power": 87.0}, "S1": {"current": 7.6}},
                          fillers=7),                                       # F5: inside default tol
    "PAST-SHORT-2017": dict(changes={"D1": {"stroke": 0.05}, "F2": {"count": 1},
                                     "L2": {"output": 900 / 60}}, fillers=4, english=True),
    "PAST-PN10-2016": dict(changes={"C1": {"pressure_class": "PN10", "temperature": 353.15},
                                    "L2": {"output": 1000 / 60}}, fillers=6),
    "PAST-NOCIP-2015": dict(changes={"L2": {"output": 800 / 60}}, drop={"C1"}, fillers=5,
                            english=True),
    "PAST-OLD-2012": dict(changes={"L1": {"protection": "IP54", "voltage": 230.0},
                                   "S1": {"noise": 78.0}, "F1": {"speed": 0.4},
                                   "L2": {"output": 600 / 60}, "L3": {"protection": "IP40"}},
                          fillers=3, english=True),
    "PAST-UNIT-2021": dict(changes={"L2": {"output": 1100 / 60}}, fillers=6, dirty_unit=True,
                           duplicate=True),                                 # dirty export
    "PAST-FAST-2024": dict(changes={}, drop={"L3"}, fillers=10),          # should item missing
}

ENGLISH = {
    "Dosierhub": "dosing stroke", "Dosiervolumen": "dose volume", "Dosiergenauigkeit": "dosing accuracy",
    "Antriebsleistung": "drive power", "Drehmoment": "torque", "Siegeltemperatur": "sealing temperature",
    "Stromaufnahme Heizung": "heater current", "Wartungsintervall": "service interval",
    "Schalldruckpegel": "sound pressure level", "Bandgeschwindigkeit": "belt speed",
    "Bandbreite": "belt width", "Stapelhöhe": "stack height", "Volumenstrom": "flow rate",
    "Reinigungstemperatur": "cleaning temperature", "Betriebsdruck": "operating pressure",
    "Druckstufe": "pressure rating", "Schutzart": "protection class",
    "Anschlussspannung": "supply voltage", "Aufstelllänge": "installed length",
    "Taktzeit": "cycle time", "Ausbringung": "output", "label speed": "label speed",
    "protection class": "protection class", "Bildschirmdiagonale": "screen size",
}

# Items a past line has that the request does not ask for (surplus; never hurts coverage)
FILLERS = [
    ("hygiene_enclosure", "Hygieneverkleidung", "length", "m", 2.4),
    ("light_curtain", "Lichtgitter", "length", "m", 1.2),
    ("lid_applicator", "Deckelaufsetzer", "frequency", "Hz", 20.0),
    ("date_coder", "Datumsdrucker", "time", "s", 0.2),
    ("reject_station", "Ausschleusung", "frequency", "Hz", 2.0),
    ("nitrogen_flush", "Stickstoffbegasung", "volume_flow", "m3/s", 0.0005),
    ("uv_sterilizer", "UV-Entkeimung", "power", "W", 400.0),
    ("check_weigher", "Kontrollwaage", "mass", "kg", 0.0002),
    ("guard_door", "Schutztür", "length", "m", 1.0),
    ("cup_printer", "Becherdrucker", "time", "s", 0.3),
]

# Module variants (10 in 5 module trees): (module, variant, {item: changes}) on BASE values
MODULES = [
    ("MOD-DOSE", "D", [("V1", {"D1": {"stroke": 0.04}}), ("V2", {"D1": {"stroke": 0.05}}),
                       ("V3", {})]),                                       # trap f: only V3
    ("MOD-SEAL", "S", [("V1", {"S1": {"current": 8.0}}), ("V2", {})]),
    ("MOD-FEED", "F", [("V1", {"F1": {"noise": 78.0}}),                    # trap l: each variant
                       ("V2", {"F2": {"stack_height": 0.5}})]),           # fails one item
    ("MOD-CIP", "C", [("V1", {})]),
    ("MOD-LINE", "L", [("V1", {"L1": {"protection": "IP54"}}), ("V2", {})]),
]

VOCABULARY = [  # house glossary: wording -> class (source empty: any source)
    ("Kompaktaggregat Hygiene", "cip_unit"), ("CIP-Anlage", "cip_unit"),
    ("Reinigungsaggregat", "cip_unit"), ("Hygieneverkleidung", "hygiene_enclosure"),
    ("Becherförderer", "conveyor"), ("Entstapler", "cup_denester"),
    ("Siegelkopf", "sealing_unit"), ("Dosierkolben", "dosing_zone"),
]
TEMPLATE_BOUND = {"noise": "at_most", "voltage": "equals"}


def write(name, header, rows):
    rows = sorted(rows, key=lambda r: [str(x) for x in r])
    with open(OUT / name, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)
    return len(rows)


def num(v):
    return "" if v is None else repr(round(v, 9)) if isinstance(v, float) else str(v)


def rfq_items():
    for st, item, cls, label, count, ms, page, attrs, manual in RFQ:
        norm = [(a[0], a[1], a[2], a[3] if len(a) > 3 else ms, a[4] if len(a) > 4 and a[4] else page)
                for a in attrs]
        yield st, item, cls, label, count, ms, page, norm, manual


def build_rfq():
    """The extracted request (JSON) and the 42-row clause sidecar."""
    children, clauses = [], []
    for st, title in STATIONS.items():
        items = []
        for s, item, cls, label, count, ms, page, attrs, manual in rfq_items():
            if s != st:
                continue
            nid = f"{RFQ_ID}.{item}"
            node = {"node_id": nid, "kind": "item", "label": label, "count": count,
                    "attributes": [{"attr_id": a, "name": n, "text": t} for a, n, t, _, _ in attrs]}
            if cls:
                node["class_id"] = cls
            items.append(node)
            clauses.append([nid, "", "item", ms, page, f"{count} x {label}"])
            for a, n, t, a_ms, a_page in attrs:
                clauses.append([nid, a, "value", a_ms, a_page, f"{n}: {t}"])
            for a, t in manual:
                clauses.append([nid, a, "manual", ms, page, t])
        children.append({"node_id": f"{RFQ_ID}.{st}", "kind": "container", "label": title,
                         "children": items})
    doc = {"tree_id": RFQ_ID, "source": "Molkerei Bergblick Demo eG", "lang": "de",
           "root": {"node_id": RFQ_ID, "kind": "container",
                    "label": "Lastenheft Becherabfüll- und Siegellinie (RFQ-2026-014)",
                    "children": children}}
    # clause ids in document order (page, then position)
    ordered = sorted(enumerate(clauses), key=lambda ic: (ic[1][4], ic[0]))
    out = []
    for n, (_, c) in enumerate(ordered, 1):
        nid, attr, kind, ms, page, text = c
        out.append([f"C-{n:02d}", page, ms, kind, nid, attr, text])
    assert len(out) == 42, len(out)
    assert sum(1 for c in out if c[2] == "must") == 37
    return doc, out


def past_values(line):
    spec = PAST[line]
    vals = {k: dict(v) for k, v in BASE.items() if k not in spec.get("drop", set())}
    for item, ch in spec["changes"].items():
        vals[item].update(ch)
    return vals


def attr_rows(tree_id, nid, item, values, english=False, dirty_unit=False):
    rows = []
    for _, it, _, _, _, _, _, attrs, _ in rfq_items():
        if it != item:
            continue
        for a, name, *_ in attrs:
            dim, unit = DIM[a]
            v = values[a]
            name = ENGLISH.get(name, name) if english else name
            if a in CATEGORY_KEYS:
                rows.append([tree_id, nid, a, name, dim, "", "", "", "provided", "", "", v])
            elif dirty_unit and a == "length":
                mm = v * 1000  # trap: the export of this line has the length in mm
                rows.append([tree_id, nid, a, name, dim, "mm", num(mm), num(mm), "provided", "", "", ""])
            else:
                rows.append([tree_id, nid, a, name, dim, unit, num(v), num(v), "provided", "", "", ""])
    return rows


def build_past(rng):
    nodes, attrs = [], []
    for line, spec in PAST.items():
        english = spec.get("english", False)
        vals = past_values(line)
        nodes.append([line, line, "", "container", "AND", f"{line} Becherlinie", "", "", 1, "internal"])
        for st, title in STATIONS.items():
            sid = f"{line}.{st}"
            nodes.append([line, sid, line, "container", "AND", title, "", "", 1, "internal"])
            for item in [i for i in BASE if i.startswith(st) and i in vals]:
                nid = f"{line}.{item}"
                nodes.append([line, nid, sid, "item", "AND", HOUSE_LABEL[item], title,
                              CLASS_OF[item], vals[item]["count"], "internal"])
                attrs += attr_rows(line, nid, item, vals[item], english, spec.get("dirty_unit", False))
        picks = rng.sample(range(len(FILLERS) * 2), spec["fillers"])
        for n, p in enumerate(sorted(picks), 1):
            cls, label, dim, unit, v = FILLERS[p % len(FILLERS)]
            nid = f"{line}.X{n:02d}"
            nodes.append([line, nid, line, "item", "AND", label, "", cls, 1, "internal"])
            val = round(v * (1 + 0.1 * (p // len(FILLERS))), 9)
            attrs.append([line, nid, "main", label, dim, unit, num(val), num(val), "provided", "", "", ""])
        if spec.get("duplicate"):  # the same row exported twice
            attrs.append(list([a for a in attrs if a[0] == line and a[2] == "power"][0]))
    return nodes, attrs


def build_modules():
    nodes, attrs = [], []
    for mod, st, variants in MODULES:
        nodes.append([mod, mod, "", "variant_group", "AND", f"Modul {STATIONS[st]}", "", "", 1, "internal"])
        nodes.append([mod, f"{mod}.V", mod, "variant_group", "OR", "Ausführung", "", "", 1, "internal"])
        for v, changes in variants:
            vid = f"{mod}.{v}"
            nodes.append([mod, vid, f"{mod}.V", "container", "OR", f"{STATIONS[st]} {v}", "", "", 1, "internal"])
            for item in [i for i in BASE if i.startswith(st)]:
                vals = dict(BASE[item])
                vals.update(changes.get(item, {}))
                nid = f"{vid}.{item}"
                nodes.append([mod, nid, vid, "item", "AND", HOUSE_LABEL[item], "", CLASS_OF[item],
                              vals["count"], "internal"])
                attrs += attr_rows(mod, nid, item, vals)
    return nodes, attrs


def meets(req, provided, dim):
    bound, r = req
    if provided is None:
        return False
    if isinstance(r, str):
        ladder = LADDERS[dim]
        return ladder.index(provided) >= ladder.index(r)
    eps = 1e-9 * max(1.0, abs(r))
    if bound == "at_least":
        return provided >= r - eps
    if bound == "at_most":
        return provided <= r + eps
    return abs(provided - r) <= eps


def build_gold(clauses):
    """Strict ground truth: every machine-checkable clause a past line does not meet."""
    by_key = {(c[4].split(".")[-1], c[5]): c for c in clauses}
    gold = []
    for line in PAST:
        vals = past_values(line)
        for _, item, _, _, count, _, _, attrs, _ in rfq_items():
            item_clause = by_key[(item, "")]
            got = vals.get(item)
            if got is None:
                gold.append([line, item_clause[0], item_clause[2], "missing item"])
                for a, *_ in attrs:
                    gold.append([line, by_key[(item, a)][0], by_key[(item, a)][2], "missing item"])
                continue
            if got["count"] < count:
                gold.append([line, item_clause[0], item_clause[2], f"count {got['count']} of {count}"])
            if item in ("D2", "D3"):  # same class: compare against the best pairing of the line
                d2, d3 = vals["D2"], vals["D3"]
                swapped = d2["power"] < d3["power"]
                got = (d3 if swapped else d2) if item == "D2" else (d2 if swapped else d3)
            for a, *_ in attrs:
                if not meets(REQ[(item, a)], got.get(a), DIM[a][0]):
                    c = by_key[(item, a)]
                    gold.append([line, c[0], c[2], "value"])
    return gold


def build_station_gold():
    """Strict ground truth per station and module variant: does the variant alone cover it?"""
    gold = []
    for mod, st, variants in MODULES:
        for v, changes in variants:
            ok = True
            for _, item, _, _, count, _, _, attrs, _ in rfq_items():
                if not item.startswith(st):
                    continue
                vals = dict(BASE[item])
                vals.update(changes.get(item, {}))
                ok &= vals["count"] >= count
                ok &= all(meets(REQ[(item, a)], vals.get(a), DIM[a][0]) for a, *_ in attrs)
            gold.append([f"{RFQ_ID}.{st}", f"{mod}.{v}", str(ok).lower()])
    return gold


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rng = random.Random(SEED)
    doc, clauses = build_rfq()
    with open(OUT / "rfq_2026_014.json", "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=1)
        f.write("\n")
    past_nodes, past_attrs = build_past(rng)
    mod_nodes, mod_attrs = build_modules()
    templates = []
    for _, item, _, _, _, _, _, attrs, _ in rfq_items():
        for a, name, *_ in attrs:
            dim, unit = DIM[a]
            bound = TEMPLATE_BOUND.get(a, REQ[(item, a)][0])
            row = [CLASS_OF[item], a, ENGLISH.get(name, name), name, dim, unit, bound, "linear"]
            if row not in templates:
                templates.append(row)
    rules = []
    for dim, ladder in LADDERS.items():
        for ri, req in enumerate(ladder):
            for pi, prov in enumerate(ladder):
                if pi >= ri:  # a higher class meets a lower requirement; a lower one never does
                    rules.append([dim, req, prov, 1.0])
    node_hdr = ["tree_id", "node_id", "parent_id", "node_kind", "edge_type", "label", "context",
                "class_id", "count", "source"]
    attr_hdr = ["tree_id", "node_id", "attr_id", "name", "dimension", "unit", "lo", "hi", "role",
                "bound", "qualifier", "category"]
    counts = {
        "clauses": write("clauses.csv", ["clause_id", "page", "must_should", "kind", "node_id",
                                         "attr_id", "text"], clauses),
        "past_line_nodes": write("past_line_nodes.csv", node_hdr, past_nodes),
        "past_line_attributes": write("past_line_attributes.csv", attr_hdr, past_attrs),
        "module_nodes": write("module_nodes.csv", node_hdr, mod_nodes),
        "module_attributes": write("module_attributes.csv", attr_hdr, mod_attrs),
        "catalogue_attributes": write("catalogue_attributes.csv", ["class_id", "attr_key", "name_en",
                                      "name_de", "dimension", "base_unit", "bound", "scale"], templates),
        "vocabulary_overrides": write("vocabulary_overrides.csv", ["wording", "source", "class_id"],
                                      [[w, "", c] for w, c in VOCABULARY]),
        "compatibility_rules": write("compatibility_rules.csv", ["dimension", "required", "provided",
                                     "score"], rules),
        "gold_deviations": write("gold_deviations.csv", ["candidate_id", "clause_id", "must_should",
                                 "reason"], build_gold(clauses)),
        "gold_station_cover": write("gold_station_cover.csv", ["station", "variant", "covers"],
                                    build_station_gold()),
    }
    print(", ".join(f"{k} {v}" for k, v in counts.items()))


if __name__ == "__main__":
    main()

"""Generate the data of examples/second_source: second sources for cast parts after a foundry exit.

    cd scripts && uv run python -m scripts.examples.second_source

A fictional pump and valve maker loses its iron foundry (Hartmann Gusstechnik). 40 cast parts
are single-sourced there. The buyer matches the part requirements (drawing extract, German and
English) against the capability sheets of 14 other foundries.

The generator knows the clean values on both sides and writes them as text the way drawings
and supplier sheets state them (units, thousands separators, old DIN grade names, ranges,
certificate wording). The ground truth (gold_second_source.csv) is computed from the clean
values with strict limits, independently of the extension. Same seed, byte-identical files.
"""

import argparse
import csv
import random
from dataclasses import dataclass, field
from pathlib import Path

SEED = 20261027
N_PARTS = 40

# Standard grade ladders (EN 1563 ductile iron, EN 1561 grey iron).
GJS = [350, 400, 450, 500, 600, 700, 800]
GJL = [150, 200, 250, 300, 350]
# old DIN 1693 / DIN 1691 names still on drawings
DIN_GRADES = {"GJS-400": "GGG-40", "GJS-500": "GGG-50", "GJS-600": "GGG-60", "GJS-700": "GGG-70",
              "GJL-150": "GG-15", "GJL-200": "GG-20", "GJL-250": "GG-25", "GJL-300": "GG-30"}
CERT_ORDER = ["2.2", "3.1", "3.2"]
PN_ORDER = [10, 16, 25, 40]


@dataclass
class Supplier:
    supplier_id: str
    name: str
    country: str
    status: str  # approved | not_approved | exiting
    max_kg: float
    envelope: tuple  # mm, any order
    grades: list  # e.g. [("GJS", 400, 700), ("GJL", 200, 300)] ranges, inclusive
    max_pn: int | None
    certs: list  # subset of CERT_ORDER; empty = no statement on the sheet
    min_lot: int
    ra_um: float
    tol_mm: float
    master_id: str = ""  # the vendor record this one duplicates
    texts: dict = field(default_factory=dict)  # per capability: (name, value_text, lang)

    def grade_set(self):
        out = set()
        for fam, lo, hi in self.grades:
            ladder = GJS if fam == "GJS" else GJL
            out |= {f"{fam}-{g}" for g in ladder if lo <= g <= hi}
        return out


# max_kg, envelope (mm), grades, max PN, certificates, min lot, best Ra (µm), best tolerance (mm)
SUPPLIERS = [
    Supplier("SUP-ALPEN", "Alpenguss AG", "AT", "approved", 300, (600, 500, 400), [("GJS", 500, 800)], 40,
             ["2.2", "3.1", "3.2"], 10, 1.6, 0.02),
    Supplier("SUP-BALTIC", "Baltic Castings UAB", "LT", "approved", 1200, (1200, 900, 600),
             [("GJL", 200, 300), ("GJS", 400, 450)], 16, [], 20, 6.3, 0.1),
    Supplier("SUP-BRANDT", "Gießerei Brandt GmbH", "DE", "approved", 1800, (1200, 1000, 600),
             [("GJS", 400, 700), ("GJL", 200, 300)], 40, ["2.2", "3.1"], 20, 3.2, 0.05),
    Supplier("SUP-BRANDT2", "Giesserei Brandt", "DE", "approved", 1800, (1000, 600, 1200),
             [("GJS", 400, 700), ("GJL", 200, 300)], 40, ["2.2", "3.1"], 20, 3.2, 0.05, master_id="SUP-BRANDT"),
    Supplier("SUP-HART", "Hartmann Gusstechnik GmbH", "DE", "exiting", 0, (0, 0, 0), [], None, [], 0, 0, 0),
    Supplier("SUP-HESSEN", "Hessische Feinguss GmbH", "DE", "approved", 25, (300, 250, 200), [("GJS", 400, 700)],
             40, ["3.1", "3.2"], 50, 0.8, 0.01),
    Supplier("SUP-KOVACS", "Kovács Öntöde Kft.", "HU", "approved", 500, (800, 600, 400),
             [("GJL", 150, 350), ("GJS", 400, 500)], 16, ["2.2", "3.1"], 50, 6.3, 0.1),
    Supplier("SUP-LOIRE", "Fonderie de la Loire SAS", "FR", "approved", 150, (500, 400, 300),
             [("GJS", 400, 700), ("GJL", 250, 250)], 40, ["3.1"], 50, 1.6, 0.02),
    Supplier("SUP-MORAVA", "Morava Slévárna s.r.o.", "CZ", "approved", 800, (900, 700, 500), [("GJS", 400, 600)],
             25, ["2.2", "3.1"], 30, 3.2, 0.05),
    Supplier("SUP-NORDGUSS", "Nordguss Werk Emden GmbH", "DE", "approved", 5000, (2500, 1800, 1200),
             [("GJS", 400, 600), ("GJL", 200, 300)], 25, ["2.2", "3.1"], 1, 6.3, 0.2),
    Supplier("SUP-NOVA", "Novaguss GmbH", "DE", "not_approved", 8000, (3000, 2000, 1500),
             [("GJS", 350, 800), ("GJL", 150, 350)], 40, ["2.2", "3.1", "3.2"], 1, 0.8, 0.01),
    Supplier("SUP-RHEIN", "Rheintal Eisenguss GmbH", "DE", "approved", 2000, (1500, 1200, 800),
             [("GJL", 200, 350), ("GJS", 400, 500)], 16, ["2.2", "3.1"], 5, 3.2, 0.05),
    Supplier("SUP-SILESIA", "Odlewnia Śląska Sp. z o.o.", "PL", "approved", 1000, (1000, 800, 500),
             [("GJL", 150, 300)], 10, ["2.2"], 100, 6.3, 0.1),
    Supplier("SUP-TAURUS", "Taurus Döküm A.Ş.", "TR", "approved", 3000, (2000, 1500, 1000),
             [("GJL", 200, 300), ("GJS", 400, 500)], 16, ["2.2", "3.1"], 100, 6.3, 0.2),
    # 37 kg: 2.6 % below a 38 kg part, inside the default 2 % log tolerance (4.7 % relative)
    Supplier("SUP-VENETO", "Fonderia Veneta S.r.l.", "IT", "approved", 37, (450, 350, 250),
             [("GJS", 400, 500), ("GJL", 250, 250)], 16, ["2.2", "3.1", "3.2"], 200, 3.2, 0.05),
]

PART_KINDS = [
    ("Pumpengehäuse", True, True),  # (description, pressure-tested, machined)
    ("Ventilgehäuse", True, True),
    ("Lagerdeckel", False, True),
    ("Getriebegehäuse", False, True),
    ("Flansch", True, True),
    ("Laufrad", False, True),
    ("Konsole", False, False),
    ("Kupplungsnabe", False, True),
    ("Gegengewicht", False, False),
    ("Spiralgehäuse", True, True),
]


@dataclass
class Part:
    part_id: str
    description: str
    kg: float
    envelope: tuple
    grade: str  # e.g. GJS-400
    pn: int | None
    cert: str | None
    lot: int
    ra_um: float | None
    tol_mm: float | None
    planted: str = ""


# --------------------------------------------------------------------------- truth


def failing_classes(p: Part, s: Supplier) -> list[str]:
    """The capability classes a supplier fails for a part, with strict limits."""
    fails = []
    if s.max_kg < p.kg:
        fails.append("piece_weight")
    for i, (need, have) in enumerate(zip(sorted(p.envelope, reverse=True), sorted(s.envelope, reverse=True))):
        if have < need:
            fails.append(f"envelope_{i + 1}")
    if p.grade not in s.grade_set():
        fails.append("material_grade")
    if p.pn is not None and (s.max_pn is None or s.max_pn < p.pn):
        fails.append("pressure_class")
    if p.cert is not None and not any(CERT_ORDER.index(c) >= CERT_ORDER.index(p.cert) for c in s.certs):
        fails.append("cert_level")
    if s.min_lot > p.lot:
        fails.append("lot_size")
    if p.ra_um is not None and s.ra_um > p.ra_um:
        fails.append("roughness")
    if p.tol_mm is not None and s.tol_mm > p.tol_mm:
        fails.append("tolerance")
    return fails


def near_miss(p: Part, s: Supplier, margin=0.06) -> bool:
    """True when a numeric limit is within `margin` of the supplier's value (either side)."""
    pairs = [(p.kg, s.max_kg), (p.lot, s.min_lot)]
    pairs += list(zip(sorted(p.envelope, reverse=True), sorted(s.envelope, reverse=True)))
    if p.ra_um is not None:
        pairs.append((p.ra_um, s.ra_um))
    if p.tol_mm is not None:
        pairs.append((p.tol_mm, s.tol_mm))
    return any(have > 0 and abs(need / have - 1) < margin for need, have in pairs)


# --------------------------------------------------------------------------- text


def de(x: float, decimals: int = 0) -> str:
    """German number: 1.200 / 0,05."""
    s = f"{x:,.{decimals}f}"
    return s.replace(",", "X").replace(".", ",").replace("X", ".")


def en(x: float, decimals: int = 0) -> str:
    return f"{x:.{decimals}f}"


def ndec(x: float) -> int:
    for d in range(4):
        if abs(round(x, d) - x) < 1e-9:
            return d
    return 4


def cert_name(c: str) -> str:
    return {"2.2": "Werkszeugnis", "3.1": "Abnahmeprüfzeugnis 3.1", "3.2": "Abnahmeprüfzeugnis 3.2"}[c]


def requirement_rows(p: Part, rng: random.Random) -> list[tuple]:
    """(attr_name, value_text, lang) as the drawing extract states them."""
    rows = []
    w = p.kg
    if w >= 1000:
        rows.append(("Stückgewicht", f"max. {de(w / 1000, ndec(w / 1000))} t", "de"))
    else:
        rows.append(rng.choice([("Stückgewicht", f"max. {de(w, ndec(w))} kg", "de"),
                                ("Rohgewicht", f"ca. {de(w, ndec(w))} kg", "de"),
                                ("piece weight", f"max. {en(w, ndec(w))} kg", "en")]))
    dims = list(p.envelope)
    rng.shuffle(dims)  # drawings state L x B x H in the drawing's orientation, not sorted
    env = " x ".join(de(d) for d in dims)
    rows.append(rng.choice([("Rohteilmaß", f"max. {env} mm", "de"), ("raw part envelope", f"{env} mm", "en")]))
    fam, num = p.grade.split("-")
    if p.planted == "din_grade":
        rows.append(("Werkstoff", DIN_GRADES[p.grade], "de"))
    elif p.planted == "elongation_variant":
        rows.append(("Werkstoff", f"EN-GJS-{num}-18", "de"))
    else:
        elongation = {350: "-22", 400: "-15", 450: "-10", 500: "-7", 600: "-3", 700: "-2", 800: "-2"}
        suffix = elongation[int(num)] if fam == "GJS" else ""
        rows.append(rng.choice([("Werkstoff", f"EN-{fam}-{num}{suffix}", "de"),
                                ("material", f"{fam}-{num}{suffix}", "en")]))
    if p.pn is not None:
        rows.append(rng.choice([("Druckprüfung", f"PN {p.pn}", "de"), ("Druckprüfung", f"PN{p.pn}", "de"),
                                ("pressure test", f"PN {p.pn}", "en")]))
    if p.cert is not None:
        if p.cert == "2.2":
            rows.append(("Prüfbescheinigung", "Werkszeugnis 2.2", "de"))
        else:
            rows.append(rng.choice([("Prüfbescheinigung", f"Abnahmeprüfzeugnis {p.cert} nach EN 10204", "de"),
                                    ("Zeugnis", f"EN 10204-{p.cert}", "de"),
                                    ("inspection certificate", f"{p.cert} acc. to EN 10204", "en")]))
    rows.append(rng.choice([("Losgröße", f"{de(p.lot)} Stk", "de"), ("lot size", f"{en(p.lot)} pcs", "en")]))
    if p.ra_um is not None:
        rows.append(rng.choice([("Oberfläche", f"Ra {de(p.ra_um, 1)} µm", "de"),
                                ("surface roughness", f"Ra {en(p.ra_um, 1)} µm", "en")]))
    if p.tol_mm is not None:
        d = ndec(p.tol_mm)
        rows.append(rng.choice([("Allgemeintoleranz", f"± {de(p.tol_mm, d)} mm", "de"),
                                ("tolerance", f"+/- {en(p.tol_mm, d)} mm", "en")]))
    if p.planted == "duplicate_line":
        rows.append(("casting weight", f"max. {en(w, ndec(w))} kg", "en"))
    if p.planted == "unmapped":
        rows.append(("Härte", "180-220 HB", "de"))
        rows.append(("Wärmebehandlung", "spannungsarm geglüht", "de"))
    return rows


def capability_rows(s: Supplier, rng: random.Random) -> list[tuple]:
    """(capability, value_text, lang) as the supplier's capability sheet states them."""
    rows = []
    if s.max_kg >= 1000:
        rows.append(rng.choice([("max. Gussgewicht", f"bis {de(s.max_kg / 1000, ndec(s.max_kg / 1000))} t", "de"),
                                ("max. Gussgewicht", f"max. {de(s.max_kg)} kg", "de")]))
    else:
        rows.append(rng.choice([("max. Gussgewicht", f"bis {de(s.max_kg)} kg", "de"),
                                ("max. casting weight", f"{en(s.max_kg)} kg", "en")]))
    env = " x ".join(de(d) for d in s.envelope)
    rows.append(rng.choice([("Formkastengröße", f"{env} mm", "de"), ("max. flask size", f"{env} mm", "en")]))
    parts = []
    for fam, lo, hi in s.grades:
        parts.append(f"{fam}-{lo}" if lo == hi else f"{fam}-{lo} … {fam}-{hi}" if rng.random() < 0.5
                     else f"EN-{fam}-{lo} bis EN-{fam}-{hi}")
    rows.append(("Werkstoffe", "; ".join(parts), "de"))
    if s.max_pn is not None:
        rows.append(rng.choice([("Druckprüfung bis", f"PN {s.max_pn}", "de"),
                                ("pressure test up to", f"PN{s.max_pn}", "en")]))
    if s.certs:
        rows.append(rng.choice([("Zeugnisse", " / ".join(s.certs), "de"),
                                ("certificates", "EN 10204 " + ", ".join(s.certs), "en")]))
    rows.append(rng.choice([("Mindestlosgröße", f"ab {de(s.min_lot)} Stk", "de"),
                            ("minimum lot", f"{en(s.min_lot)} pcs", "en")]))
    rows.append(rng.choice([("erreichbare Rauheit", f"Ra {de(s.ra_um, 1)} µm", "de"),
                            ("achievable roughness", f"Ra {en(s.ra_um, 1)} µm", "en")]))
    d = ndec(s.tol_mm)
    rows.append(rng.choice([("Toleranzklasse", f"± {de(s.tol_mm, d)} mm", "de"),
                            ("achievable tolerance", f"+/- {en(s.tol_mm, d)} mm", "en")]))
    return rows


# --------------------------------------------------------------------------- master data

# class_id, kind, dimension, bound (the direction a requirement of the class needs), ladder, description.
# material_grade is the planted category without a dimension: the person who keeps this sheet left it blank.
CAPABILITY_CLASSES = [
    ("cert_level", "category", "cert_level", "", "ordinal", "inspection certificate EN 10204 (2.2 < 3.1 < 3.2)"),
    ("envelope_1", "numeric", "length", "at_least", "", "largest raw-part dimension (flask must be at least as long)"),
    ("envelope_2", "numeric", "length", "at_least", "", "middle raw-part dimension"),
    ("envelope_3", "numeric", "length", "at_least", "", "smallest raw-part dimension"),
    ("lot_size", "numeric", "count", "at_most", "", "supplier minimum lot must not exceed the part's lot"),
    ("material_grade", "category", "", "", "nominal", "cast iron grade EN 1563 / EN 1561, without elongation"),
    ("piece_weight", "numeric", "mass", "at_least", "", "supplier maximum casting weight must reach the part weight"),
    ("pressure_class", "category", "pressure_class", "", "ordinal", "pressure test PN (PN10 < PN16 < PN25 < PN40)"),
    ("roughness", "numeric", "length", "at_most", "", "achievable Ra must not exceed the drawing's Ra"),
    ("tolerance", "numeric", "length", "at_most", "", "achievable general tolerance (half-width)"),
]

# wording -> class, for the drawing extract (part) and the capability sheets (supplier).
# 'Härte' and 'Wärmebehandlung' are deliberately missing: the audit lists them.
VOCABULARY = [
    ("Rohteilmaß", "part", "envelope"), ("raw part envelope", "part", "envelope"),
    ("Formkastengröße", "supplier", "envelope"), ("max. flask size", "supplier", "envelope"),
    ("Stückgewicht", "part", "piece_weight"), ("Rohgewicht", "part", "piece_weight"),
    ("piece weight", "part", "piece_weight"), ("casting weight", "part", "piece_weight"),
    ("max. Gussgewicht", "supplier", "piece_weight"), ("max. casting weight", "supplier", "piece_weight"),
    ("Werkstoff", "part", "material_grade"), ("material", "part", "material_grade"),
    ("Werkstoffe", "supplier", "material_grade"),
    ("Druckprüfung", "part", "pressure_class"), ("pressure test", "part", "pressure_class"),
    ("Druckprüfung bis", "supplier", "pressure_class"), ("pressure test up to", "supplier", "pressure_class"),
    ("Prüfbescheinigung", "part", "cert_level"), ("Zeugnis", "part", "cert_level"),
    ("inspection certificate", "part", "cert_level"),
    ("Zeugnisse", "supplier", "cert_level"), ("certificates", "supplier", "cert_level"),
    ("Losgröße", "part", "lot_size"), ("lot size", "part", "lot_size"),
    ("Mindestlosgröße", "supplier", "lot_size"), ("minimum lot", "supplier", "lot_size"),
    ("Oberfläche", "part", "roughness"), ("surface roughness", "part", "roughness"),
    ("erreichbare Rauheit", "supplier", "roughness"), ("achievable roughness", "supplier", "roughness"),
    ("Allgemeintoleranz", "part", "tolerance"), ("tolerance", "part", "tolerance"),
    ("Toleranzklasse", "supplier", "tolerance"), ("achievable tolerance", "supplier", "tolerance"),
]


def compatibility_rules() -> list[tuple]:
    """Ordinal ladders written out: a higher provided level meets a lower required one (9 rows)."""
    rows = []
    for i, lo in enumerate(PN_ORDER):
        rows += [("pressure_class", f"PN{lo}", f"PN{hi}", 1.0) for hi in PN_ORDER[i + 1:]]
    for i, lo in enumerate(CERT_ORDER):
        rows += [("cert_level", f"EN10204-{lo}", f"EN10204-{hi}", 1.0) for hi in CERT_ORDER[i + 1:]]
    return rows


# --------------------------------------------------------------------------- parts


def planted_parts() -> list[Part]:
    """Parts with no approved second source (and one that only a near miss would cover)."""
    return [
        # 3.2 certificate at 400 kg: ALPEN and HESSEN issue 3.2 but cannot pour 400 kg
        Part("", "Ventilgehäuse", 400, (700, 520, 380), "GJS-500", 40, "3.2", 20, 6.3, 0.1, "no_source"),
        # 3.2 and 120 kg, but 650 mm long: ALPEN's flask is 600 mm
        Part("", "Pumpengehäuse", 120, (650, 400, 300), "GJS-500", 25, "3.2", 50, 3.2, 0.05, "no_source"),
        # 6 t: only the unapproved Novaguss pours more than 5 t
        Part("", "Spiralgehäuse", 6000, (2200, 1600, 1100), "GJS-400", 16, "3.1", 2, 12.5, 0.5, "no_source"),
        # GJS-800 at 500 kg: ALPEN pours GJS-800 only up to 300 kg
        Part("", "Laufrad", 500, (800, 800, 300), "GJS-800", None, "3.1", 20, 3.2, 0.1, "no_source"),
        # 2.8 m long: no approved flask is longer than 2.5 m
        Part("", "Konsole", 1500, (2800, 900, 500), "GJL-250", None, None, 5, None, None, "no_source"),
        # Ra 0.8 µm at 200 kg: HESSEN reaches 0.8 µm but pours 25 kg at most
        Part("", "Laufrad", 200, (500, 500, 250), "GJS-500", None, "3.1", 50, 0.8, 0.05, "no_source"),
        # 38 kg, GJS-450, 3.2: only VENETO fits, and its 37 kg limit misses by 2.6 %
        Part("", "Ventilgehäuse", 38, (420, 320, 240), "GJS-450", 16, "3.2", 200, 3.2, 0.05, "near_miss"),
    ]


def random_part(rng: random.Random, approved: list[Supplier]) -> Part:
    """A part some approved supplier can make: sample inside a target supplier's capability."""
    while True:
        target = rng.choice(approved)
        desc, pressure, machined = rng.choice(PART_KINDS)
        kg = round(target.max_kg * rng.uniform(0.15, 0.85), 0 if target.max_kg > 60 else 1)
        env = tuple(int(round(d * rng.uniform(0.4, 0.88) / 10) * 10) for d in sorted(target.envelope, reverse=True))
        grade = rng.choice(sorted(target.grade_set()))
        pn = None
        if pressure and target.max_pn is not None:
            pn = rng.choice([p for p in PN_ORDER if p <= target.max_pn])
        cert = rng.choice(target.certs) if target.certs and rng.random() < 0.85 else None
        lot = target.min_lot * rng.choice([2, 3, 5, 10]) if target.min_lot > 1 else rng.choice([2, 5, 10])
        ra = tol = None
        if machined:
            ra = rng.choice([r for r in (1.6, 3.2, 6.3, 12.5) if r >= target.ra_um * 1.5] or [12.5])
            tol = rng.choice([t for t in (0.05, 0.1, 0.2, 0.5) if t >= target.tol_mm * 1.5] or [0.5])
        p = Part("", desc, kg, env, grade, pn, cert, lot, ra, tol)
        if not failing_classes(p, target) and not any(near_miss(p, s) for s in approved):
            return p


def build(seed: int):
    rng = random.Random(seed)
    approved = [s for s in SUPPLIERS if s.status == "approved" and not s.master_id]
    parts = [random_part(rng, approved) for _ in range(N_PARTS - len(planted_parts()))]
    # data traps on parts that have a source
    fits = {"din_grade": lambda p: p.grade in DIN_GRADES, "elongation_variant": lambda p: p.grade == "GJS-400"}
    for trap in ["din_grade", "duplicate_line", "unmapped", "elongation_variant", "din_grade", "unmapped",
                 "duplicate_line"]:
        p = next(p for p in parts[3:] if not p.planted and fits.get(trap, lambda p: True)(p))
        p.planted = trap
    parts += planted_parts()
    rng.shuffle(parts)
    for i, p in enumerate(parts):
        p.part_id = f"P-{1001 + i}"
    return parts, approved


def write_csv(path: Path, header: list[str], rows: list[tuple]):
    with path.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description="Generate the data of examples/second_source")
    ap.add_argument("--seed", type=int, default=SEED)
    ap.add_argument("--output", type=Path,
                    default=Path(__file__).resolve().parents[4] / "examples" / "second_source" / "data")
    args = ap.parse_args()
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    parts, approved = build(args.seed)
    text_rng = random.Random(args.seed + 1)

    write_csv(out / "parts.csv", ["part_id", "description", "material_group", "current_supplier_id", "annual_qty"],
              [(p.part_id, p.description, p.grade.split("-")[0], "SUP-HART", p.lot * text_rng.choice([2, 4, 6, 12]))
               for p in parts])

    req_rows = []
    for p in parts:
        for i, (name, text, lang) in enumerate(requirement_rows(p, text_rng)):
            req_rows.append((p.part_id, f"{p.part_id}-{i + 1:02d}", name, text, lang))
    write_csv(out / "part_requirements.csv", ["part_id", "req_id", "attr_name", "value_text", "lang"], req_rows)

    write_csv(out / "suppliers.csv", ["supplier_id", "name", "country", "status"],
              [(s.supplier_id, s.name, s.country, s.status) for s in SUPPLIERS])

    cap_rows = []
    for s in SUPPLIERS:
        if s.status == "exiting":
            continue
        for name, text, lang in capability_rows(s, text_rng):
            cap_rows.append((s.supplier_id, name, text, lang))
    write_csv(out / "supplier_capabilities.csv", ["supplier_id", "capability", "value_text", "lang"], cap_rows)

    write_csv(out / "capability_classes.csv", ["class_id", "kind", "dimension", "bound", "ladder", "description"],
              CAPABILITY_CLASSES)
    write_csv(out / "attribute_vocabulary.csv", ["wording", "side", "class_id"], sorted(VOCABULARY))
    write_csv(out / "compatibility_rules.csv", ["dimension", "required", "provided", "score"], compatibility_rules())

    # ground truth over every approved vendor (master records only)
    gold = []
    for p in parts:
        for s in approved:
            fails = failing_classes(p, s)
            gold.append((p.part_id, s.supplier_id, "false" if fails else "true", ";".join(fails)))
    write_csv(out / "gold_second_source.csv", ["part_id", "supplier_id", "covers", "failing_classes"], gold)

    n_src = sum(1 for p in parts if any(not failing_classes(p, s) for s in approved))
    print(f"{len(parts)} parts, {len(req_rows)} requirement rows, {len(cap_rows)} capability rows; "
          f"{n_src} parts have an approved second source -> {out}")


if __name__ == "__main__":
    main()

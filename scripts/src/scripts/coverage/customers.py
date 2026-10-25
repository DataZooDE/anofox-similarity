"""Customer master with planted duplicates (HANDOVER §3.7).

Eight fictional vehicle-maker groups -> legal entities -> plants -> purchasing
contacts. About 8 % of the records are planted duplicates of another record:
spelling variants, umlaut transliteration, legal-form variants, moved plants
and shared tax IDs. A few near misses share a street with another company but
are different entities.
"""

import zlib

import numpy as np

from .domain import MAKERS

HEADER = ["record_id", "record_type", "parent_record_id", "group_name", "company_name", "legal_form",
          "plant_name", "street", "house_no", "postal_code", "city", "country", "tax_id", "contact_name",
          "email"]

CITIES = [("Würzburg", "97076"), ("Göttingen", "37073"), ("Fürth", "90762"), ("Lüneburg", "21335"),
          ("Mönchengladbach", "41179"), ("Düren", "52351"), ("Bühl", "77815"), ("Kassel", "34117"),
          ("Ulm", "89077"), ("Zwickau", "08056"), ("Regensburg", "93055"), ("Saarlouis", "66740"),
          ("Gießen", "35390"), ("Köln", "50735"), ("Osnabrück", "49090"), ("Heilbronn", "74076")]
STREETS = ["Industriestraße", "Werkstraße", "Am Güterbahnhof", "Hafenstraße", "Gewerbering",
           "Robert-Koch-Straße", "Lindenallee", "Mühlenweg", "Südring", "Schlossstraße", "Römerstraße",
           "Am Wäldchen", "Brückenstraße", "Hüttenweg"]
ENTITY_SUFFIX = ["Fahrzeugwerk", "Komponenten", "Antriebstechnik", "Karosseriebau", "Einkauf",
                 "Logistik", "Interieur"]
LEGAL = ["GmbH", "AG", "GmbH & Co. KG", "SE"]
LEGAL_VARIANT = {"GmbH": "G.m.b.H.", "AG": "Aktiengesellschaft", "GmbH & Co. KG": "GmbH u. Co. KG",
                 "SE": "S.E."}
FIRST = ["Anna", "Jörg", "Björn", "Sünje", "Käthe", "Lukas", "Mehmet", "Sofia", "Günther", "Paula",
         "Matthias", "Özlem", "Jürgen", "Lea", "Tobias", "Marek"]
LAST = ["Müller", "Schäfer", "Köhler", "Weiß", "Groß", "Lehmann", "Krüger", "Hofmann", "Brückner",
        "Vogt", "Böhm", "Schröder", "Nowak", "Haas", "Fuchs", "Möller"]
UMLAUT = str.maketrans({"ä": "ae", "ö": "oe", "ü": "ue", "Ä": "Ae", "Ö": "Oe", "Ü": "Ue", "ß": "ss"})


def _rng(seed, name):
    return np.random.Generator(np.random.PCG64([seed, zlib.crc32(name.encode())]))


def _pick(rng, seq):
    return seq[int(rng.integers(len(seq)))]


def _typo(rng, s: str) -> str:
    """Swap two different neighbouring letters or drop one; never returns s unchanged."""
    letters = [i for i, ch in enumerate(s) if ch.isalpha()]
    i = _pick(rng, letters[1:-1] or letters)
    if rng.random() < 0.5 and i + 1 < len(s) and s[i] != s[i + 1]:
        return s[:i] + s[i + 1] + s[i] + s[i + 2:]
    return s[:i] + s[i + 1:]


def build(seed: int):
    rng = _rng(seed, "customers")
    rows, clusters = [], []
    n = 0

    def add(rec, cluster=None, kind="", lookalike=""):
        nonlocal n
        n += 1
        rid = f"C{n:05d}"
        row = [rid] + [rec.get(h, "") for h in HEADER[1:]]
        rows.append(row)
        clusters.append([rid, cluster or rid, kind, lookalike])
        return rid, row

    originals = {"legal_entity": [], "plant": [], "contact": []}
    for _, group, code in MAKERS:
        gid, _ = add({"record_type": "group", "group_name": group, "company_name": group, "country": "DE"})
        for e in range(int(rng.integers(4, 8))):
            city, plz = _pick(rng, CITIES)
            legal = _pick(rng, LEGAL)
            company = f"{group.split()[0]} {ENTITY_SUFFIX[e % len(ENTITY_SUFFIX)]}"
            tax = f"DE{int(rng.integers(100_000_000, 999_999_999))}"
            ent = {"record_type": "legal_entity", "parent_record_id": gid, "group_name": group,
                   "company_name": company, "legal_form": legal, "street": _pick(rng, STREETS),
                   "house_no": str(int(rng.integers(1, 120))), "postal_code": plz, "city": city, "country": "DE",
                   "tax_id": tax}
            eid, erow = add(ent)
            originals["legal_entity"].append((eid, ent))
            for p in range(int(rng.integers(6, 12))):
                pcity, pplz = _pick(rng, CITIES)
                plant = {"record_type": "plant", "parent_record_id": eid, "group_name": group,
                         "company_name": company, "legal_form": legal,
                         "plant_name": f"Werk {pcity} {p + 1}", "street": _pick(rng, STREETS),
                         "house_no": str(int(rng.integers(1, 200))), "postal_code": pplz, "city": pcity,
                         "country": "DE", "tax_id": tax}
                pid, _ = add(plant)
                originals["plant"].append((pid, plant))
                for _ in range(int(rng.integers(2, 6))):
                    first, last = _pick(rng, FIRST), _pick(rng, LAST)
                    contact = dict(plant, record_type="contact", parent_record_id=pid,
                                   contact_name=f"{first} {last}",
                                   email=f"{first.translate(UMLAUT).lower()}.{last.translate(UMLAUT).lower()}"
                                         f"@{code.lower()}-demo.example")
                    cid, _ = add(contact)
                    originals["contact"].append((cid, contact))

    base = len(rows)
    n_dups = int(round(base * 0.08 / 0.92))
    kinds = ["spelling", "umlaut", "legal_form", "moved_plant", "shared_tax_id"]
    for i in range(n_dups):
        kind = kinds[i % len(kinds)]
        pool = originals["plant"] if kind in ("moved_plant", "shared_tax_id") else \
            originals["legal_entity"] + originals["plant"] + originals["contact"]
        if kind == "umlaut":
            pool = [o for o in pool if any(ch in "äöüÄÖÜß" for ch in "".join(str(v) for v in o[1].values()))]
        oid, orig = _pick(rng, pool)
        dup = dict(orig)
        if kind == "spelling":
            field = "contact_name" if dup["record_type"] == "contact" else "company_name"
            dup[field] = _typo(rng, dup[field])
        elif kind == "umlaut":
            for f in ("company_name", "plant_name", "street", "city", "contact_name"):
                dup[f] = str(dup.get(f, "")).translate(UMLAUT)
        elif kind == "legal_form":
            dup["legal_form"] = LEGAL_VARIANT[dup["legal_form"]]
        elif kind == "moved_plant":
            dup["street"] = _pick(rng, [s for s in STREETS if s != dup["street"]])
            dup["house_no"] = str(int(rng.integers(1, 200)))
        elif kind == "shared_tax_id":
            words = dup["company_name"].split()
            dup["company_name"] = f"{words[0][0]}. {' '.join(words[1:])}"
            dup["plant_name"] = dup["plant_name"].replace("Werk", "Standort")
        add(dup, cluster=oid, kind=kind)

    # near misses: a different company at the same address as an existing plant
    for i in range(12):
        oid, orig = _pick(rng, originals["plant"])
        other = {"record_type": "legal_entity", "group_name": "", "company_name":
                 f"{_pick(rng, ['Nordlicht', 'Talblick', 'Feldmark', 'Kranich'])} {_pick(rng, ['Logistik', 'Metallbau', 'Kunststofftechnik'])}",
                 "legal_form": _pick(rng, LEGAL), "street": orig["street"], "house_no": orig["house_no"],
                 "postal_code": orig["postal_code"], "city": orig["city"], "country": "DE",
                 "tax_id": f"DE{int(rng.integers(100_000_000, 999_999_999))}"}
        add(other, kind="near_miss", lookalike=oid)
    return rows, clusters

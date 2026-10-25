"""Deterministic synthetic vectors (no model), HANDOVER §3.6.

Single vectors:  vec = normalise(class_centroid + phrasing_offset + source_boilerplate + noise)

* The source boilerplate is strong and leans towards two "hub" classes per
  source, so raw cosine sends items to the hubs. Per-source mean-centring
  removes it.
* Held-out (test) phrasings have larger offsets than development phrasings, so
  single-vector matching degrades on unseen wording.
* Token vectors (late interaction): each phrasing is a few concept tokens that
  lean towards the classes using them. Catalogue exemplars are the concept
  tokens of the development phrasings, so MaxSim finds held-out phrasings
  through shared concepts.
* Attribute-name vectors share a strong common direction, giving a raw cosine
  floor of about 0.85 between unrelated names.
"""

import json
import zlib

import numpy as np

from .domain import CLASSES, INTERNAL_SOURCE, MAKERS, PHRASINGS, VOCABULARY

DIM = 64

# calibration constants (tuned once, then fixed)
BOILERPLATE = 1.2
HUB_WEIGHT = 1.1
DEV_OFFSET = 1.0
TEST_OFFSET = 2.0
NOISE = 2.2
CONCEPT_PULL = 0.9
TOKEN_NOISE = 0.12
NAME_COMMON, NAME_KEY, NAME_NOISE = 0.85, 0.135, 0.015


def _rng(seed: int, name: str) -> np.random.Generator:
    return np.random.Generator(np.random.PCG64([seed, zlib.crc32(name.encode())]))


def _unit(v: np.ndarray) -> np.ndarray:
    return v / np.linalg.norm(v)


def _rand_unit(seed: int, name: str) -> np.ndarray:
    return _unit(_rng(seed, name).standard_normal(DIM))


def enc(v: np.ndarray, digits: int) -> str:
    return json.dumps([round(float(x), digits) + 0.0 for x in v], separators=(",", ":"))


def enc_tokens(T: list[np.ndarray]) -> str:
    return "[" + ",".join(enc(t, 3) for t in T) + "]"


class Space:
    def __init__(self, seed: int):
        self.seed = seed
        self.class_ids = [c.class_id for c in CLASSES]
        self.mu = {c: _rand_unit(seed, f"centroid:{c}") for c in self.class_ids}
        # two hub classes per source
        self.boiler = {}
        sources = [m[0] for m in MAKERS] + [INTERNAL_SOURCE]
        for i, s in enumerate(sources):
            h1, h2 = self.class_ids[(3 * i) % len(self.class_ids)], self.class_ids[(3 * i + 7) % len(self.class_ids)]
            self.boiler[s] = _unit(_rand_unit(seed, f"boiler:{s}") + HUB_WEIGHT * (self.mu[h1] + self.mu[h2]))
        # concept tokens lean towards the classes whose development phrasings use
        # them; concepts seen only in held-out phrasings get no pull (no label leak)
        users = {}
        for cid in sorted(PHRASINGS):
            for ph in PHRASINGS[cid]["dev"]:
                for c in ph.concepts:
                    users.setdefault(c, set()).add(cid)
        self.concept = {c: _unit(CONCEPT_PULL * _unit(sum(self.mu[k] for k in sorted(ks)))
                                 + _rand_unit(seed, f"concept:{c}"))
                        for c, ks in sorted(users.items())}

    def item_vec(self, cls: str, offset_key: str, offset_scale: float, source: str, noise_key: str) -> np.ndarray:
        v = (self.mu[cls] + offset_scale * _rand_unit(self.seed, f"offset:{offset_key}")
             + BOILERPLATE * self.boiler[source] + NOISE * _rand_unit(self.seed, f"noise:{noise_key}"))
        return _unit(v)

    def token(self, name: str, occurrence: str) -> np.ndarray:
        base = self.concept.get(name)
        if base is None:
            base = _rand_unit(self.seed, f"token:{name}")
        return _unit(base + TOKEN_NOISE * _rand_unit(self.seed, f"tok-noise:{occurrence}"))


def phrasing_concepts(cid: str, split: str, text: str) -> tuple[str, ...]:
    for ph in PHRASINGS[cid][split]:
        if ph.text == text:
            return ph.concepts
    raise KeyError(text)


def write_all(out, seed: int, write_csv, requests, unseen, pids) -> None:
    sp = Space(seed)
    by_id = {pid: (cid, split, ph) for (cid, split, _), (pid, ph) in pids.items()}

    # catalogue: class vector plus the concept tokens of its development phrasings
    cat_rows = []
    for cid in sp.class_ids:
        v = _unit(sp.mu[cid] + 0.15 * _rand_unit(seed, f"catalogue:{cid}"))
        dev_ids = sorted(pid for pid, (c, s, _) in by_id.items() if c == cid and s == "dev")
        concepts = sorted({c for pid in dev_ids for c in by_id[pid][2].concepts})
        cat_rows.append([cid, enc(v, 4), enc_tokens([sp.concept[c] for c in concepts]), json.dumps(dev_ids)])
    write_csv(out, "catalogue_vectors", ["class_id", "vec", "exemplar_tokens", "exemplar_phrasing_ids"], cat_rows)

    # request items
    rows = []
    for req in requests:
        maker = req["programme"].maker
        for ri in req["items"]:
            key = f"{req['rid']}/{ri.node_id}"
            v = sp.item_vec(ri.class_id, ri.phrasing_id, DEV_OFFSET, maker, key)
            names = list(by_id[ri.phrasing_id][2].concepts) + [f"frame:{maker}:1", f"frame:{maker}:2"]
            names += [f"attr:{a.tmpl.key}" for a in ri.attrs]
            names = names[:12]
            toks = [sp.token(n, f"{key}/{i}") for i, n in enumerate(names)]
            rows.append([req["rid"], ri.node_id, maker, enc(v, 4), enc_tokens(toks)])
    write_csv(out, "request_item_vectors", ["tree_id", "node_id", "source", "vec", "tokens"], rows)

    # unseen requirements: held-out phrasings, larger offsets
    rows = []
    for u in unseen:
        v = sp.item_vec(u["class_id"], u["phrasing_id"], TEST_OFFSET, u["source"], u["item_id"])
        names = list(u["phrasing"].concepts) + [f"frame:{u['source']}:1", f"frame:{u['source']}:2"]
        toks = [sp.token(n, f"{u['item_id']}/{i}") for i, n in enumerate(names)]
        rows.append([u["item_id"], u["source"], enc(v, 4), enc_tokens(toks)])
    write_csv(out, "unseen_vectors", ["item_id", "source", "vec", "tokens"], rows)

    # internal vocabulary: misleading wordings sit next to their apparent class
    rows = []
    for w in VOCABULARY:
        shown = w.apparent_class_id or w.class_id
        v = _unit(sp.mu[shown] + (0.35 * sp.mu[w.class_id] if w.misleading else 0)
                  + 0.25 * _rand_unit(seed, f"wording:{w.text}") + BOILERPLATE * sp.boiler[INTERNAL_SOURCE]
                  + 0.15 * _rand_unit(seed, f"wnoise:{w.text}"))
        dev = PHRASINGS[shown]["dev"]
        concepts = [dev[0].concepts[0], dev[1].concepts[0]]
        names = concepts + [f"internal:{w.text}", "frame:internal"]
        toks = [sp.token(n, f"{w.text}/{i}") for i, n in enumerate(names)]
        rows.append([w.text, INTERNAL_SOURCE, enc(v, 4), enc_tokens(toks)])
    write_csv(out, "vocabulary_vectors", ["wording", "source", "vec", "tokens"], rows)

    # attribute names: request wordings and internal (German template) names
    names = {}
    for c in CLASSES:
        for t in c.attrs:
            for n in (t.name_en, t.name_de) + t.names_en + t.names_de:
                names.setdefault(n, t.key)
    common = _rand_unit(seed, "name:common")
    rows = []
    for n in sorted(names):
        key = names[n]
        v = _unit(np.sqrt(NAME_COMMON) * common + np.sqrt(NAME_KEY) * _rand_unit(seed, f"name-key:{key}")
                  + np.sqrt(NAME_NOISE) * _rand_unit(seed, f"name:{n}"))
        rows.append([n, key, enc(v, 4)])
    write_csv(out, "attribute_name_vectors", ["name", "attr_key", "vec"], rows)

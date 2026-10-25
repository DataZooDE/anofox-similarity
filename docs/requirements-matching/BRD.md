# BRD — Requirement-to-Offer Matching on Hierarchical Business Data

| | |
|---|---|
| **Document** | Business Requirements Document |
| **Scope** | anofox-similarity, next feature set ("coverage matching") |
| **Status** | Draft for review |
| **Date** | October 2026 |
| **Companion** | [HLD.md](HLD.md) — High-Level Design |

---

## 1. Executive summary

anofox-similarity answers one question well: **"how alike are A and B?"** (Jaccard over
components, WL kernel, text/transaction embeddings, fusion). Most matching work in
companies asks a different question:

> **"Does B satisfy what A asks for — and where exactly does it fall short?"**

A customer specification against the product portfolio, a bill of materials against
the parts library, a request for quotation against past offers, a spare part against its
alternates, a job profile against skills. All of these compare a **request**
with an **offer**. Both are **semi-structured and hierarchical**: sections, assemblies
and groups contain items, items carry attributes, and attributes carry numbers with
units and limits.

This BRD proposes extending anofox-similarity from symmetric similarity to
**asymmetric, explainable coverage matching** on hierarchical data. Symmetric
similarity stays for deduplication and predecessor search. The design comes from an
internal benchmark study of the full chain: request → shared vocabulary → offer →
past solution. The main findings, independent of domain:

1. **Match through a shared vocabulary (a "pivot").** Request and offer are first mapped
   onto a common catalogue of functions or classes. Direct text similarity between the two
   sides collapsed accuracy to 10–38 %.
2. **The question is directional.** Symmetric Jaccard penalises an offer for providing
   *more* than requested. Containment (`Σ min / Σ request`) is as cheap and lifted
   top-1 by +11 points at the ceiling.
3. **Numbers need rules, not embeddings.** The unit is a hard gate, the value is checked
   with a direction (*at least / at most / within*), and the attribute name only discounts.
   Name similarity is unreliable (exact translations scored 0.02 after calibration).
4. **Embedding whole records fails.** One vector per record, WL signatures and slot tensors
   all lost 40+ points of top-1 to plain set overlap over class identifiers.
   Embeddings pay off at short text leaves.
5. **Data curation beats model work.** A few misread internal terms (3 of 55) touched
   83.5 % of all records. A one-hour vocabulary review was worth +21 points, more than
   any model change.
6. **Two stages work: cheap recall, then explainable re-ranking.** The output is a
   calibrated score in [0, 1] (mean absolute error ≈ 0.04 against ground truth). A
   threshold of 0.9 handled two thirds of the decisions automatically at 95 % precision.

## 2. Business context and problem

### 2.1 The recurring pattern

| Business process | Request side | Offer side | Typical question |
|---|---|---|---|
| Quote / tender response | Customer specification, RFQ | Product portfolio, past orders | "Which product or past solution covers ≥ 90 % of this spec, and what is missing?" |
| Engineer-to-order / design reuse | New BOM or requirement list | Archive of designs, module library | "Build new or adapt existing? Which existing design is closest *and* sufficient?" |
| Part substitution / alternates | Part with technical attributes | Parts catalogue | "Which alternates meet or exceed every limit?" |
| Supplier qualification | Required certifications, capacities, ranges | Supplier master data | "Which suppliers can deliver all positions?" |
| Master-data quality (customer, supplier, material) | Record | Same table | "Which records describe the same entity?" *(symmetric)* |
| Skills / resource matching | Role or work-package profile | People or teams | "Who covers the required skills at the required level?" |

### 2.2 Why today's tooling falls short

- **Full-text or embedding search** finds documents that *sound* alike. It does not check
  whether a 5 A requirement is met by a 3 A offer, or tell 5 A from 5 V.
- **Symmetric set similarity** (today's `jaccard_similarity`) ranks a lean partial match
  above a richer offer that fully covers the request.
- **Flat comparison** ignores hierarchy. Variants (OR) are averaged instead of picking the
  best one. Grouping levels (sections, assemblies) that differ between sources break the
  comparison.
- **Scores are not actionable.** A planner or an AI agent needs "80 % covered; requirement
  4.2 fails on max. current; requirement 7 has no counterpart", not an opaque cosine.
- **Internal jargon is invisible to generic models.** Company-specific wordings are a
  small, closed set, and they are the main source of error.

### 2.3 Business goals

| ID | Goal | Measure |
|---|---|---|
| G1 | Cut the time to answer "do we already have something that fits?" from expert hours to seconds | Latency of the top-k answer on 10⁴–10⁵ candidates < 1 s (stage 1), < 5 s (stage 2 on top-k) |
| G2 | Automate the clear cases, route the rest | Share of decisions above the auto-accept threshold at ≥ 95 % precision |
| G3 | Make every result explainable and auditable | 100 % of stage-2 results carry a per-requirement trace and gap list |
| G4 | Domain-agnostic, SQL-native | No domain-specific code; the same functions serve BOM, customer, supplier, part and skill data |
| G5 | Keep humans in the loop where it pays | Review queues ranked by impact (frequency × uncertainty), not by volume |

## 3. Stakeholders and users

| Role | Interest |
|---|---|
| Sales / pre-sales engineer | Fast fit assessment of incoming specifications; gap list for the offer |
| Product / portfolio manager | Coverage of the portfolio against market demand; which gaps recur |
| Design / development engineer | Reuse of existing designs; build-vs-adapt decision |
| Master-data steward | Deduplication; curation of the internal vocabulary |
| Procurement | Alternates and second sources; supplier capability matching |
| Data / AI engineer | SQL building blocks, calibrated scores, evaluation tooling |
| AI agent (LLM) | Deterministic candidate generation plus traces it can check and act on |

## 4. Scope

### 4.1 In scope

1. **Asymmetric and weighted set measures** (containment, Tversky, weighted Jaccard) over
   generic `(entity, feature, weight)` data.
2. **Typed property-tree data model** for hierarchical records, with AND/OR edges, transparent
   container levels, and typed attribute leaves (name, quantity/unit, value interval, role, bound).
3. **Quantity normalisation** (units, dimensions, ranges, bound words) and
   **direction-aware value comparison**.
4. **Coverage kernel** (stage 2): calibrated score, best path, attribute-level trace and gaps;
   best-match and partition assignment modes; extensive (summed) counts.
5. **Scalable recall** (stage 1) for set measures, including containment.
6. **Catalogue pivot**: soft classification of free text onto a catalogue, with margin and
   review routing.
7. **Curated vocabulary and compatibility tables**: one-time human confirmation of internal
   wordings; ordinal/categorical compatibility rules.
8. **Evaluation and calibration helpers**: recall@k, MRR, regret, threshold-at-precision,
   bootstrap intervals.
9. **Entity clustering** of pairwise matches for master-data deduplication.

### 4.2 Out of scope (this release)

- Document parsing and extraction (PDF, Office, tables) into trees. The extension *consumes*
  trees; extraction stays upstream (LLM with structured output, ETL).
- Calling LLMs from inside the extension. The extension *prepares* candidates and margins
  for an external re-rank.
- Training or hosting embedding models. The extension accepts vectors supplied by the user
  (`FLOAT[]`) and may use the existing `embed_text` where it is configured.
- Domain-specific catalogues, unit tables beyond SI + common business units, or rule packs.
- Optimisation-grade solvers (MILP). Capacity-constrained assignment uses greedy and
  min-cost-flow heuristics only.

## 5. Business requirements

Priority: **M** = must, **S** = should, **C** = could.

### 5.1 Matching semantics

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-01 | The user must be able to choose **symmetric** (similarity) or **asymmetric** (coverage of request by offer) semantics explicitly; no hidden default switch. | M | Dedup is symmetric; requirement matching is not. A wrong direction cost 11 points. |
| BR-02 | Set-based measures must support **weights/counts** (quantities, number of channels, BOM quantity). | M | "20 × single + 1 × 18-way" must count as 38, not as 2 distinct items. |
| BR-03 | Ties among fully covering candidates should be broken in favour of the **leaner** candidate (least surplus). | S | Avoid over-specified offers when several fully cover the request. |
| BR-04 | Hierarchical records must be comparable **regardless of depth**. Grouping levels must not have to align. | M | Request and offer hierarchies are never organised the same way. |
| BR-05 | Alternatives (variants, configurations, options) must be scored by their **best** member, not the average. | M | An offer with one fitting variant fits. |
| BR-06 | The user must be able to forbid **substitution**: one offer item may not silently cover a requirement of a different function. | S | Substitution inflated coverage and caused the worst ranking errors. |

### 5.2 Attributes and values

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-10 | Values must be normalised to a base unit per physical/business dimension (SI, currency, time, percent, count). | M | "2500 mA" vs "2.5 A"; "6 weeks" vs "42 days". |
| BR-11 | Values of different dimensions must **never** match (hard gate). | M | 5 V must never satisfy 5 A. |
| BR-12 | Each requirement value must be compared **with a direction**: at least, at most, within, equals. | M | The direction-aware rule improved selection by 2.6 points and cut score error by one third. |
| BR-13 | The comparison must report both a **pass score** and a **distance to compliance**. | M | "Misses by 3 %" is actionable; "0" is not. |
| BR-14 | The user must be able to choose log or linear scale per dimension. | S | Physical ranges span decades; prices and dates do not. |
| BR-15 | Categorical/ordinal attributes (grades, certification levels, classes) must use **curated compatibility tables**, not text similarity. | S | Lexical similarity ≠ technical compatibility. |
| BR-16 | Attribute **names** may contribute only a bounded discount, never decide a match. | M | Name similarity is unreliable across languages and jargon. |

### 5.3 Explainability and output

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-20 | Every stage-2 result must return a score in [0, 1] meaning "share of the request covered". | M | Business users think in "80 % / 100 % covered". |
| BR-21 | Every stage-2 result must return a **trace**: per requirement, the matched offer path, per-attribute scores, and the failing attributes. | M | Auditability; agent plausibility checks. |
| BR-22 | Requirements without any counterpart must be listed as **gaps**. | M | The gap list is the input to the offer or to development. |
| BR-23 | Outputs must be relational (SQL tables), so they can be joined, filtered and exported. | M | SQL-native positioning. |

### 5.4 Vocabulary and human-in-the-loop

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-30 | Free-text items must be classifiable onto a user-supplied catalogue as a **probability distribution**, with a top-1/top-2 margin. | M | Soft labels keep the right answer in play when the classifier is unsure. |
| BR-31 | The system must produce a **review queue** of distinct internal wordings, ranked by impact (frequency × uncertainty). | M | A small, closed vocabulary was the dominant error source; ranking by impact fixes the most with the fewest questions. |
| BR-32 | Confirmed mappings must **override** the classifier deterministically. | M | Reviewed labels unlock containment and partition (+31 points end to end). |
| BR-33 | Low-margin decisions must be exportable for an external re-rank (LLM or human) with the top-k candidates. | S | Re-ranking only the uncertain 15 % added 3–4 points at low cost. |

### 5.5 Scale and operations

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-40 | Stage 1 must scan 10⁵ candidates in < 1 s on a single node; sub-linear recall should be available beyond that. | M | Interactive use; large archives. |
| BR-41 | Stage 2 must evaluate top-k (k ≤ 50) in < 5 s. | M | Interactive use. |
| BR-42 | All features must work on DuckDB 1.5.6 and 2.0 and must not require optional extensions. | M | Current support matrix. |
| BR-43 | Pairwise match results must be clusterable into entities (connected components) natively, with optional acceleration via DuckPGQ. | S | Master-data dedup. |

### 5.6 Quality assurance

| ID | Requirement | Prio | Rationale |
|---|---|---|---|
| BR-50 | Provide evaluation functions: recall@k, MRR, regret, calibration error, threshold-at-precision. | M | Teams must be able to prove fitness on their own data. |
| BR-51 | Provide bootstrap confidence intervals clustered by the unit that varies (e.g., wording, request). | S | The study found single-split figures optimistic (88.7 % on development wording vs 74.5 % on unseen wording). |
| BR-52 | Provide a weight-fitting helper once ≥ a few hundred reviewed matches exist. | C | Parameters are a-priori defaults until then. |
| BR-53 | Ship a deterministic, seeded synthetic benchmark (request/catalogue/offer/past-solution trees) in the test suite. | S | Regression safety for scoring changes. |

## 6. Success criteria (acceptance at business level)

On the shipped synthetic benchmark, with reviewed vocabulary:

| KPI | Target |
|---|---|
| Stage-1 recall@20 of the best candidate (containment) | ≥ 0.90 |
| Stage 1+2 top-1 = best candidate | ≥ 0.85 |
| Mean regret of the top-1 pick (coverage points) | ≤ 0.01 |
| Mean absolute error of predicted vs true coverage | ≤ 0.05 |
| Unmeetable requests flagged (score < 0.95) | ≥ 95 % |
| Precision at the default auto-accept threshold (0.9) | ≥ 95 % |

On customer data, the same KPIs are measured against 3–5 expert-confirmed end-to-end
examples per use case **before** any parameter tuning (pre-registered gate).

## 7. Assumptions, constraints, risks

| # | Item | Mitigation |
|---|---|---|
| A1 | Upstream extraction delivers trees with separate name / unit / value / bound fields. | Document the target schema; provide a JSON → tree loader. |
| A2 | A catalogue (pivot vocabulary) exists or can be drafted (10–200 entries typical). | Catalogue-mining helpers are a later phase. |
| C1 | DuckDB extensions cannot call each other natively. | Everything core is self-contained; DuckPGQ is used only through optional SQL recipes. |
| C2 | Embedding quality varies; the extension does not ship models. | Bring-your-own-vectors API; the pivot uses calibration and centring to stay robust. |
| R1 | Containment on **unreviewed** labels ranks worse than Jaccard (−5.5 points). | Default stays symmetric for recall unless a reviewed vocabulary is registered; documented loudly. |
| R2 | A hard partition on noisy labels amplifies errors (−21 to −27 points). | Partition only for candidates whose wordings are all reviewed; otherwise fall back to best match automatically. |
| R3 | Synthetic results overstate real performance. | Pre-registered gates on real examples (§6). |
| R4 | One offer item may legitimately serve two requirements in reality. | Partition is optional; capacity-aware assignment is a later phase. |

## 8. Release phasing (business view)

| Phase | Business capability | Unlocks |
|---|---|---|
| **R1 — Directional sets** | Containment / Tversky / weighted measures; generic feature-multiset input; tie-breaks | "Which archived items fully contain this request?" on existing BOM/feature data, immediately |
| **R2 — Typed values** | Unit normalisation; direction-aware comparison; compatibility tables | Correct "meets the limit?" checks on attributes |
| **R3 — Coverage kernel** | Property trees; stage-2 coverage with trace and gaps; partition mode | Explainable "80 % covered, these 3 gaps" answers |
| **R4 — Pivot and curation** | Soft classification onto a catalogue; margins; review queue; overrides | Robust mapping of free text and internal jargon; human-in-the-loop |
| **R5 — Scale and QA** | Sub-linear recall (weighted MinHash, containment LSH); evaluation and calibration; entity clustering | Large archives; measurable trust; dedup |

## 9. Glossary

| Term | Meaning |
|---|---|
| **Request / query tree** | What is asked for: specification, BOM to source, role profile. |
| **Offer / candidate tree** | What may satisfy it: product, design, part, supplier, person. |
| **Catalogue / pivot** | Shared vocabulary of classes (functions, capabilities, categories) that both sides are mapped onto before comparison. |
| **Anchor node** | A node compared across trees (class instance or attribute). Container nodes (sections, groups) are transparent. |
| **AND / OR edge** | Composition (all children needed) vs alternatives (best child counts). |
| **Containment** | Share of the request contained in the offer: `Σ min(q, c) / Σ q`. |
| **Coverage** | Share of the request's requirements satisfied, in [0, 1], including value checks. |
| **Gap** | A requirement with no satisfying counterpart. |
| **Regret** | Coverage of the best candidate minus coverage of the candidate ranked first. |
| **Margin** | Probability difference between the top-1 and top-2 class; low margin means uncertain. |

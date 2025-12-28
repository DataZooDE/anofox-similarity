#!/usr/bin/env python3
"""
Synthetic BOM test data generator for anofox-similarity TDD.
Generates controlled test scenarios with known ground truth.

Extended for Phase 2 with weekly time series and predecessor patterns.

Usage:
    python generate_test_data.py --output test/data/synthetic/ --seed 42
"""

import csv
import gzip
import random
import argparse
import math
from dataclasses import dataclass, field
from typing import List, Set, Optional
from pathlib import Path
from datetime import date, timedelta
import json


@dataclass(frozen=True)
class Material:
    id: str
    description: str
    material_group: str
    material_type: str
    created_date: str = "2020-01-01"


@dataclass
class BOMItem:
    bom_id: str
    parent_id: str
    child_id: str
    quantity: float
    level: int
    position: int


@dataclass
class GroundTruth:
    material_a: str
    material_b: str
    expected_similarity: float
    relationship_type: str
    notes: str = ""


@dataclass
class GoodsMovement:
    material_id: str
    movement_date: str
    quantity: float
    movement_type: str = "261"  # SAP goods issue


class SyntheticBOMGenerator:
    def __init__(self, seed: int = 42):
        random.seed(seed)
        self.seed = seed
        self.materials: List[Material] = []
        self.bom_items: List[BOMItem] = []
        self.ground_truth: List[GroundTruth] = []
        self.goods_movements: List[GoodsMovement] = []

        self.prefixes = ['Premium', 'Standard', 'Heavy-Duty', 'Precision', 'Industrial']
        self.materials_vocab = ['Steel', 'Aluminum', 'Polymer', 'Copper', 'Composite']
        self.components = ['Housing', 'Bracket', 'Assembly', 'Module', 'Frame']

    def _generate_description(self) -> str:
        return f"{random.choice(self.prefixes)} {random.choice(self.materials_vocab)} {random.choice(self.components)}"

    def _create_material(self, mat_id: str, mat_type: str, created_date: str = "2020-01-01") -> Material:
        return Material(
            id=mat_id,
            description=self._generate_description(),
            material_group=f"GRP-{random.randint(1, 30):02d}",
            material_type=mat_type,
            created_date=created_date
        )

    def _generate_weekly_time_series(
        self,
        material_id: str,
        start_date: date,
        weeks: int,
        base_demand: float,
        trend: float = 0.0,
        seasonality_amplitude: float = 0.2,
        noise_level: float = 0.1,
        ramp_up_weeks: int = 0,
        ramp_down_weeks: int = 0
    ) -> None:
        """Generate weekly goods movements with realistic demand pattern.

        Args:
            material_id: Material to generate movements for
            start_date: Start date of time series
            weeks: Number of weeks to generate
            base_demand: Base weekly demand quantity
            trend: Linear trend per week (positive = growth)
            seasonality_amplitude: Amplitude of seasonal pattern (0-1)
            noise_level: Random noise level (0-1)
            ramp_up_weeks: Weeks for demand to ramp up from 0
            ramp_down_weeks: Weeks for demand to ramp down to 0 (from end)
        """
        for week in range(weeks):
            current_date = start_date + timedelta(weeks=week)

            # Base demand with trend
            demand = base_demand + (trend * week)

            # Seasonal pattern (annual, peak in Q4)
            week_of_year = current_date.isocalendar()[1]
            seasonal_factor = 1.0 + seasonality_amplitude * math.sin(2 * math.pi * (week_of_year - 13) / 52)
            demand *= seasonal_factor

            # Random noise
            noise = 1.0 + random.uniform(-noise_level, noise_level)
            demand *= noise

            # Ramp up pattern (new product introduction)
            if ramp_up_weeks > 0 and week < ramp_up_weeks:
                ramp_factor = week / ramp_up_weeks
                demand *= ramp_factor

            # Ramp down pattern (product phase-out)
            if ramp_down_weeks > 0 and week >= (weeks - ramp_down_weeks):
                weeks_from_end = weeks - week
                ramp_factor = weeks_from_end / ramp_down_weeks
                demand *= ramp_factor

            # Ensure non-negative
            demand = max(0, round(demand, 2))

            if demand > 0:
                self.goods_movements.append(GoodsMovement(
                    material_id=material_id,
                    movement_date=current_date.isoformat(),
                    quantity=demand
                ))

    def _generate_predecessor_time_series(
        self,
        predecessor_id: str,
        successor_id: str,
        transition_date: date,
        weeks_before: int = 52,
        weeks_after: int = 52,
        base_demand: float = 500.0,
        overlap_weeks: int = 8
    ) -> None:
        """Generate anti-correlated time series for predecessor-successor pair.

        The predecessor declines as the successor ramps up, creating negative correlation.
        """
        # Predecessor: active before transition, phases out during overlap
        pred_start = transition_date - timedelta(weeks=weeks_before)
        self._generate_weekly_time_series(
            material_id=predecessor_id,
            start_date=pred_start,
            weeks=weeks_before + overlap_weeks,
            base_demand=base_demand,
            trend=-2.0,  # Slight decline
            ramp_down_weeks=overlap_weeks + 4
        )

        # Successor: starts ramping up around transition, fully active after
        succ_start = transition_date - timedelta(weeks=overlap_weeks // 2)
        self._generate_weekly_time_series(
            material_id=successor_id,
            start_date=succ_start,
            weeks=overlap_weeks + weeks_after,
            base_demand=base_demand,
            trend=1.5,  # Growth
            ramp_up_weeks=overlap_weeks + 4
        )

    # =========================================================================
    # S1-S6: Original scenarios from CONCEPT.md
    # =========================================================================

    def generate_baseline_identical(self) -> None:
        """S1: Two products with identical BOMs."""
        components = [self._create_material(f"IDENT-COMP-{i:03d}", "HALB") for i in range(5)]
        self.materials.extend(components)

        prod_a = self._create_material("IDENT-PROD-A", "FERT")
        prod_b = self._create_material("IDENT-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for prod in [prod_a, prod_b]:
            for i, comp in enumerate(components):
                self.bom_items.append(BOMItem(
                    bom_id=f"BOM-{prod.id}",
                    parent_id=prod.id,
                    child_id=comp.id,
                    quantity=round(random.uniform(1, 5), 2),
                    level=1,
                    position=i * 10
                ))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=1.0,
            relationship_type='identical',
            notes='Identical component sets'
        ))

        # Generate time series for both
        self._generate_weekly_time_series(prod_a.id, date(2022, 1, 1), 104, 100)
        self._generate_weekly_time_series(prod_b.id, date(2022, 1, 1), 104, 95)

    def generate_baseline_disjoint(self) -> None:
        """S2: Two products with no overlap."""
        components_a = [self._create_material(f"DISJ-A-COMP-{i:03d}", "HALB") for i in range(5)]
        components_b = [self._create_material(f"DISJ-B-COMP-{i:03d}", "HALB") for i in range(5)]
        self.materials.extend(components_a + components_b)

        prod_a = self._create_material("DISJ-PROD-A", "FERT")
        prod_b = self._create_material("DISJ-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for i, comp in enumerate(components_a):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, comp.id, 1.0, 1, i*10))
        for i, comp in enumerate(components_b):
            self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, comp.id, 1.0, 1, i*10))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=0.0,
            relationship_type='unrelated',
            notes='Completely disjoint component sets'
        ))

        # Generate time series
        self._generate_weekly_time_series(prod_a.id, date(2021, 1, 1), 156, 200)
        self._generate_weekly_time_series(prod_b.id, date(2021, 6, 1), 130, 180)

    def generate_controlled_overlap(self) -> None:
        """S3: Products with known overlap ratio (Jaccard = 3/7)."""
        shared = [self._create_material(f"OVLP-SHARED-{i:03d}", "HALB") for i in range(3)]
        unique_a = [self._create_material(f"OVLP-UNIQ-A-{i:03d}", "HALB") for i in range(2)]
        unique_b = [self._create_material(f"OVLP-UNIQ-B-{i:03d}", "HALB") for i in range(2)]
        self.materials.extend(shared + unique_a + unique_b)

        prod_a = self._create_material("OVLP-PROD-A", "FERT")
        prod_b = self._create_material("OVLP-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for i, comp in enumerate(shared + unique_a):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, comp.id, 1.0, 1, i*10))
        for i, comp in enumerate(shared + unique_b):
            self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, comp.id, 1.0, 1, i*10))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=3/7,
            relationship_type='partial_overlap',
            notes='3 shared of 7 unique components (Jaccard = 3/7)'
        ))

        # Generate time series
        self._generate_weekly_time_series(prod_a.id, date(2022, 1, 1), 104, 80)
        self._generate_weekly_time_series(prod_b.id, date(2023, 7, 1), 26, 50)

    def generate_variant_cluster(self, base_id: str, n_variants: int = 3, n_components: int = 10) -> None:
        """S4: Product with variants at different similarity levels."""
        base_components = [self._create_material(f"{base_id}-COMP-{i:03d}", "HALB") for i in range(n_components)]
        self.materials.extend(base_components)

        base_product = self._create_material(f"{base_id}-BASE", "FERT")
        self.materials.append(base_product)

        for i, comp in enumerate(base_components):
            self.bom_items.append(BOMItem(f"BOM-{base_product.id}", base_product.id, comp.id, 1.0, 1, i*10))

        retention_rates = [0.9, 0.7, 0.5]
        for v, retention in enumerate(retention_rates[:n_variants]):
            variant = self._create_material(f"{base_id}-VAR-{v+1}", "FERT")
            self.materials.append(variant)

            n_keep = int(n_components * retention)
            kept = random.sample(base_components, n_keep)
            n_new = n_components - n_keep
            new_comps = [self._create_material(f"{base_id}-VAR{v+1}-NEW-{i:03d}", "HALB") for i in range(n_new)]
            self.materials.extend(new_comps)

            for i, comp in enumerate(kept + new_comps):
                self.bom_items.append(BOMItem(f"BOM-{variant.id}", variant.id, comp.id, 1.0, 1, i*10))

            expected_jaccard = n_keep / (2 * n_components - n_keep)
            self.ground_truth.append(GroundTruth(
                material_a=base_product.id,
                material_b=variant.id,
                expected_similarity=expected_jaccard,
                relationship_type='variant',
                notes=f'{retention*100:.0f}% component retention'
            ))

        # Generate time series for base and variants
        self._generate_weekly_time_series(base_product.id, date(2020, 1, 1), 208, 300)
        history_months = [78, 104, 26]  # VAR-1: 18mo, VAR-2: 24mo, VAR-3: 6mo
        for v in range(n_variants):
            variant_id = f"{base_id}-VAR-{v+1}"
            weeks = history_months[v]
            start = date(2022, 7, 1) if v == 0 else (date(2022, 1, 1) if v == 1 else date(2023, 7, 1))
            self._generate_weekly_time_series(variant_id, start, weeks, 250 - v*50)

    def generate_hierarchical_bom(self) -> None:
        """S5: Multi-level BOM structure with sub-assemblies."""
        # Create leaf components (4 per sub-assembly, 3 sub-assemblies = 12 leaves)
        leaves = []
        for subasm_idx in range(3):  # A, B, C
            subasm_leaves = [
                self._create_material(f"HIER-LEAF-{subasm_idx}-{i:02d}", "HALB")
                for i in range(4)
            ]
            leaves.extend(subasm_leaves)
        self.materials.extend(leaves)

        # Create sub-assemblies
        subasm_names = ['A', 'B', 'C']
        sub_assemblies = []
        for idx, name in enumerate(subasm_names):
            subasm = self._create_material(f"HIER-SUBASM-{name}", "HALB")
            sub_assemblies.append(subasm)
            self.materials.append(subasm)

            # Link sub-assembly to its 4 leaves
            for i in range(4):
                leaf = leaves[idx * 4 + i]
                self.bom_items.append(BOMItem(
                    f"BOM-{subasm.id}", subasm.id, leaf.id, 1.0, 1, i*10
                ))

        # Create 3 products with different sub-assembly combinations
        # PROD-1: A + B
        # PROD-2: A + C
        # PROD-3: B + C
        product_configs = [
            ("HIER-PROD-1", [0, 1]),  # A, B
            ("HIER-PROD-2", [0, 2]),  # A, C
            ("HIER-PROD-3", [1, 2]),  # B, C
        ]

        for prod_id, subasm_indices in product_configs:
            product = self._create_material(prod_id, "FERT")
            self.materials.append(product)

            for pos, idx in enumerate(subasm_indices):
                self.bom_items.append(BOMItem(
                    f"BOM-{prod_id}", prod_id, sub_assemblies[idx].id, 1.0, 1, pos*10
                ))

        # Ground truth: each pair shares 1 of 3 sub-assemblies = Jaccard 1/3
        self.ground_truth.append(GroundTruth(
            "HIER-PROD-1", "HIER-PROD-2", 1/3, 'hierarchical',
            'Share HIER-SUBASM-A'
        ))
        self.ground_truth.append(GroundTruth(
            "HIER-PROD-1", "HIER-PROD-3", 1/3, 'hierarchical',
            'Share HIER-SUBASM-B'
        ))
        self.ground_truth.append(GroundTruth(
            "HIER-PROD-2", "HIER-PROD-3", 1/3, 'hierarchical',
            'Share HIER-SUBASM-C'
        ))

    def generate_predecessor_chain(self, chain_length: int = 4, retention: float = 0.7) -> None:
        """S6: Succession chain for predecessor inference testing."""
        base_components = [self._create_material(f"PRED-CHAIN-COMP-{i:03d}", "HALB") for i in range(10)]
        self.materials.extend(base_components)

        current_components: Set[Material] = set(base_components)
        products = []

        for gen in range(chain_length):
            created = f"202{gen}-01-01"
            product = self._create_material(f"PRED-CHAIN-GEN-{gen:02d}", "FERT", created)
            self.materials.append(product)
            products.append(product)

            for i, comp in enumerate(current_components):
                self.bom_items.append(BOMItem(f"BOM-{product.id}", product.id, comp.id, 1.0, 1, i*10))

            n_keep = int(len(current_components) * retention)
            kept = set(random.sample(list(current_components), n_keep))
            n_new = len(current_components) - n_keep
            new_comps = [self._create_material(f"PRED-CHAIN-GEN{gen+1}-NEW-{i:03d}", "HALB") for i in range(n_new)]
            self.materials.extend(new_comps)
            current_components = kept | set(new_comps)

        # Ground truth for predecessor relationships
        for i in range(1, len(products)):
            # Calculate expected Jaccard: n_keep / (2*n - n_keep) = 7/13 ≈ 0.5385
            expected_jaccard = round(7 / 13, 4)
            self.ground_truth.append(GroundTruth(
                material_a=products[i].id,
                material_b=products[i-1].id,
                expected_similarity=expected_jaccard,
                relationship_type='predecessor',
                notes=f'Direct predecessor (Gen {i-1} -> Gen {i})'
            ))

        # Generate anti-correlated time series for predecessor chain
        for i in range(chain_length):
            transition = date(2020 + i, 7, 1)
            if i == 0:
                # First generation: active 2020, phases out 2021
                self._generate_weekly_time_series(
                    products[i].id, date(2020, 1, 1), 78, 500,
                    trend=-3.0, ramp_down_weeks=26
                )
            elif i == chain_length - 1:
                # Last generation: ramps up, stays active
                self._generate_weekly_time_series(
                    products[i].id, date(2020 + i - 1, 10, 1), 78, 500,
                    trend=2.0, ramp_up_weeks=20
                )
            else:
                # Middle generations: ramp up, then phase out
                self._generate_weekly_time_series(
                    products[i].id, date(2020 + i - 1, 10, 1), 78, 500,
                    ramp_up_weeks=16, ramp_down_weeks=20
                )

    # =========================================================================
    # S7-S12: New Phase 2 scenarios
    # =========================================================================

    def generate_predecessor_anticorrelation(self) -> None:
        """S7: Clear predecessor-successor with strong anti-correlation."""
        # Create materials with 80% overlap
        shared = [self._create_material(f"S7-SHARED-{i:03d}", "HALB") for i in range(8)]
        unique_old = [self._create_material(f"S7-OLD-UNIQ-{i:03d}", "HALB") for i in range(2)]
        unique_new = [self._create_material(f"S7-NEW-UNIQ-{i:03d}", "HALB") for i in range(2)]
        self.materials.extend(shared + unique_old + unique_new)

        old_prod = self._create_material("S7-PRED-OLD", "FERT", "2021-01-01")
        new_prod = self._create_material("S7-PRED-NEW", "FERT", "2023-01-01")
        self.materials.extend([old_prod, new_prod])

        for i, comp in enumerate(shared + unique_old):
            self.bom_items.append(BOMItem(f"BOM-{old_prod.id}", old_prod.id, comp.id, 1.0, 1, i*10))
        for i, comp in enumerate(shared + unique_new):
            self.bom_items.append(BOMItem(f"BOM-{new_prod.id}", new_prod.id, comp.id, 1.0, 1, i*10))

        # Jaccard = 8 / (10 + 10 - 8) = 8/12 = 0.6667
        self.ground_truth.append(GroundTruth(
            material_a=new_prod.id,
            material_b=old_prod.id,
            expected_similarity=8/12,
            relationship_type='predecessor',
            notes='S7: Clear predecessor with anti-correlation'
        ))

        # Generate anti-correlated time series
        self._generate_predecessor_time_series(
            old_prod.id, new_prod.id,
            transition_date=date(2023, 1, 1),
            weeks_before=104, weeks_after=52,
            base_demand=400, overlap_weeks=12
        )

    def generate_gradual_phaseout(self) -> None:
        """S8: Gradual phase-out with extended overlap period."""
        shared = [self._create_material(f"S8-SHARED-{i:03d}", "HALB") for i in range(7)]
        unique_old = [self._create_material(f"S8-OLD-UNIQ-{i:03d}", "HALB") for i in range(3)]
        unique_new = [self._create_material(f"S8-NEW-UNIQ-{i:03d}", "HALB") for i in range(3)]
        self.materials.extend(shared + unique_old + unique_new)

        old_prod = self._create_material("S8-PHASE-OLD", "FERT", "2020-06-01")
        new_prod = self._create_material("S8-PHASE-NEW", "FERT", "2022-06-01")
        self.materials.extend([old_prod, new_prod])

        for i, comp in enumerate(shared + unique_old):
            self.bom_items.append(BOMItem(f"BOM-{old_prod.id}", old_prod.id, comp.id, 1.0, 1, i*10))
        for i, comp in enumerate(shared + unique_new):
            self.bom_items.append(BOMItem(f"BOM-{new_prod.id}", new_prod.id, comp.id, 1.0, 1, i*10))

        # Jaccard = 7/13
        self.ground_truth.append(GroundTruth(
            material_a=new_prod.id,
            material_b=old_prod.id,
            expected_similarity=7/13,
            relationship_type='predecessor',
            notes='S8: Gradual phase-out with 6-month overlap'
        ))

        # Extended overlap period
        self._generate_predecessor_time_series(
            old_prod.id, new_prod.id,
            transition_date=date(2022, 9, 1),
            weeks_before=130, weeks_after=78,
            base_demand=350, overlap_weeks=26  # 6-month overlap
        )

    def generate_no_predecessor(self) -> None:
        """S9: Genuinely new product with no predecessor (low similarity to all)."""
        # Unique components not shared with any existing material
        unique_comps = [self._create_material(f"S9-UNIQUE-{i:03d}", "HALB") for i in range(8)]
        self.materials.extend(unique_comps)

        new_prod = self._create_material("S9-BRAND-NEW", "FERT", "2024-01-01")
        self.materials.append(new_prod)

        for i, comp in enumerate(unique_comps):
            self.bom_items.append(BOMItem(f"BOM-{new_prod.id}", new_prod.id, comp.id, 1.0, 1, i*10))

        # No ground truth - this product has no predecessor
        # The test should verify empty result from infer_predecessors()

        # New product just starting
        self._generate_weekly_time_series(
            new_prod.id, date(2024, 1, 1), 52, 200,
            trend=5.0, ramp_up_weeks=12
        )

    def generate_wl_structure_test(self) -> None:
        """S10: Same components, different assembly structure for WL kernel testing."""
        # Create shared leaf components
        leaves = [self._create_material(f"S10-LEAF-{i:02d}", "HALB") for i in range(6)]
        self.materials.extend(leaves)

        # Product A: Linear chain structure (A -> B -> C)
        # Sub-assembly contains first 3 leaves
        subasm_a = self._create_material("S10-SUBASM-LINEAR", "HALB")
        self.materials.append(subasm_a)
        for i in range(3):
            self.bom_items.append(BOMItem(f"BOM-{subasm_a.id}", subasm_a.id, leaves[i].id, 1.0, 1, i*10))

        prod_a = self._create_material("S10-PROD-LINEAR", "FERT")
        self.materials.append(prod_a)
        self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, subasm_a.id, 1.0, 1, 10))
        for i in range(3, 6):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, leaves[i].id, 1.0, 1, (i-2)*10))

        # Product B: Star structure (hub with 6 spokes)
        prod_b = self._create_material("S10-PROD-STAR", "FERT")
        self.materials.append(prod_b)
        for i, leaf in enumerate(leaves):
            self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, leaf.id, 1.0, 1, i*10))

        # Ground truth: same leaves but different structure
        # Jaccard at leaf level = 1.0 (same components)
        # WL kernel should give lower similarity due to structural difference
        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=1.0,  # Jaccard (flat)
            relationship_type='structural_diff',
            notes='S10: Same components, different structure - WL should differ from Jaccard'
        ))

    def generate_deep_hierarchy_test(self) -> None:
        """S11: Deep BOM hierarchy for WL iteration testing."""
        # 4-level deep BOM
        # Level 0: Final product
        # Level 1: 2 major assemblies
        # Level 2: 4 sub-assemblies (2 per major)
        # Level 3: 8 leaf components (2 per sub)

        leaves = [self._create_material(f"S11-L3-{i:02d}", "HALB") for i in range(8)]
        self.materials.extend(leaves)

        sub_assemblies = []
        for i in range(4):
            subasm = self._create_material(f"S11-L2-{i:02d}", "HALB")
            sub_assemblies.append(subasm)
            self.materials.append(subasm)
            # Each sub-assembly has 2 leaves
            for j in range(2):
                leaf_idx = i * 2 + j
                self.bom_items.append(BOMItem(f"BOM-{subasm.id}", subasm.id, leaves[leaf_idx].id, 1.0, 2, j*10))

        major_assemblies = []
        for i in range(2):
            major = self._create_material(f"S11-L1-{i:02d}", "HALB")
            major_assemblies.append(major)
            self.materials.append(major)
            # Each major assembly has 2 sub-assemblies
            for j in range(2):
                sub_idx = i * 2 + j
                self.bom_items.append(BOMItem(f"BOM-{major.id}", major.id, sub_assemblies[sub_idx].id, 1.0, 1, j*10))

        # Create two products with different major assembly combinations
        prod_a = self._create_material("S11-PROD-DEEP-A", "FERT")
        prod_b = self._create_material("S11-PROD-DEEP-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        # PROD-A uses both major assemblies
        for i, major in enumerate(major_assemblies):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, major.id, 1.0, 0, i*10))

        # PROD-B uses only first major assembly + 2 direct leaves
        self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, major_assemblies[0].id, 1.0, 0, 10))
        self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, leaves[6].id, 1.0, 0, 20))
        self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, leaves[7].id, 1.0, 0, 30))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=0.5,  # Approximate - share 1 of 2 major assemblies
            relationship_type='deep_hierarchy',
            notes='S11: Deep 4-level BOM for WL iteration testing'
        ))

    def generate_transitive_chain(self) -> None:
        """S12: Extended predecessor chain for transitive relationship testing."""
        # 6-generation chain with 70% retention per generation
        chain_length = 6
        retention = 0.7
        n_components = 10

        base_comps = [self._create_material(f"S12-BASE-{i:03d}", "HALB") for i in range(n_components)]
        self.materials.extend(base_comps)

        current_comps: Set[Material] = set(base_comps)
        products = []

        for gen in range(chain_length):
            created = f"201{8+gen}-01-01"
            product = self._create_material(f"S12-GEN-{gen:02d}", "FERT", created)
            self.materials.append(product)
            products.append(product)

            for i, comp in enumerate(current_comps):
                self.bom_items.append(BOMItem(f"BOM-{product.id}", product.id, comp.id, 1.0, 1, i*10))

            # Prepare next generation
            n_keep = int(len(current_comps) * retention)
            kept = set(random.sample(list(current_comps), n_keep))
            n_new = len(current_comps) - n_keep
            new_comps = [self._create_material(f"S12-GEN{gen+1}-NEW-{i:03d}", "HALB") for i in range(n_new)]
            self.materials.extend(new_comps)
            current_comps = kept | set(new_comps)

        # Ground truth: only direct predecessors should have high confidence
        for i in range(1, len(products)):
            expected_jaccard = round(7 / 13, 4)  # 70% retention
            self.ground_truth.append(GroundTruth(
                material_a=products[i].id,
                material_b=products[i-1].id,
                expected_similarity=expected_jaccard,
                relationship_type='predecessor',
                notes=f'S12: Gen {i-1} -> Gen {i} (direct predecessor)'
            ))

        # Generate time series with clear succession patterns
        for i, product in enumerate(products):
            # Staggered transitions every 12 months
            start = date(2018 + i, 1, 1)
            if i == 0:
                self._generate_weekly_time_series(product.id, start, 78, 400, ramp_down_weeks=30)
            elif i == chain_length - 1:
                self._generate_weekly_time_series(product.id, start, 104, 450, ramp_up_weeks=20)
            else:
                self._generate_weekly_time_series(product.id, start, 78, 420, ramp_up_weeks=16, ramp_down_weeks=26)

    # =========================================================================
    # Main generation and save methods
    # =========================================================================

    def generate_all_scenarios(self) -> None:
        """Generate all test scenarios S1-S12."""
        print("Generating S1: Identical BOMs...")
        self.generate_baseline_identical()

        print("Generating S2: Disjoint BOMs...")
        self.generate_baseline_disjoint()

        print("Generating S3: Controlled Overlap...")
        self.generate_controlled_overlap()

        print("Generating S4: Variant Clusters...")
        for i in range(5):
            self.generate_variant_cluster(f"FAMILY-{i:02d}")

        print("Generating S5: Hierarchical BOM...")
        self.generate_hierarchical_bom()

        print("Generating S6: Predecessor Chain...")
        self.generate_predecessor_chain(chain_length=4, retention=0.7)

        print("Generating S7: Predecessor Anti-correlation...")
        self.generate_predecessor_anticorrelation()

        print("Generating S8: Gradual Phase-out...")
        self.generate_gradual_phaseout()

        print("Generating S9: No Predecessor...")
        self.generate_no_predecessor()

        print("Generating S10: WL Structure Test...")
        self.generate_wl_structure_test()

        print("Generating S11: Deep Hierarchy Test...")
        self.generate_deep_hierarchy_test()

        print("Generating S12: Transitive Chain...")
        self.generate_transitive_chain()

    def save(self, output_dir: str) -> None:
        """Save generated data to gzipped CSV files."""
        path = Path(output_dir)
        path.mkdir(parents=True, exist_ok=True)

        import io

        def write_gzipped_csv(filepath: Path, header: list, rows: list):
            """Write rows to a gzipped CSV file."""
            buffer = io.StringIO()
            writer = csv.writer(buffer)
            writer.writerow(header)
            writer.writerows(rows)
            with gzip.open(filepath, 'wt', encoding='utf-8') as f:
                f.write(buffer.getvalue())

        write_gzipped_csv(
            path / "materials.csv.gz",
            ['material_id', 'description', 'material_group', 'material_type', 'created_date'],
            [[m.id, m.description, m.material_group, m.material_type, m.created_date] for m in self.materials]
        )

        write_gzipped_csv(
            path / "bom_items.csv.gz",
            ['bom_id', 'parent_id', 'child_id', 'quantity', 'level', 'position'],
            [[item.bom_id, item.parent_id, item.child_id, item.quantity, item.level, item.position] for item in self.bom_items]
        )

        write_gzipped_csv(
            path / "ground_truth.csv.gz",
            ['material_a', 'material_b', 'expected_similarity', 'relationship_type', 'notes'],
            [[gt.material_a, gt.material_b, round(gt.expected_similarity, 4), gt.relationship_type, gt.notes] for gt in self.ground_truth]
        )

        write_gzipped_csv(
            path / "goods_movements.csv.gz",
            ['material_id', 'movement_date', 'quantity', 'movement_type'],
            [[gm.material_id, gm.movement_date, gm.quantity, gm.movement_type] for gm in self.goods_movements]
        )

        metadata = {
            'seed': self.seed,
            'n_materials': len(self.materials),
            'n_bom_items': len(self.bom_items),
            'n_ground_truth': len(self.ground_truth),
            'n_goods_movements': len(self.goods_movements),
            'scenarios': ['S1-identical', 'S2-disjoint', 'S3-overlap', 'S4-variants',
                         'S5-hierarchical', 'S6-predecessor', 'S7-anticorrelation',
                         'S8-phaseout', 'S9-no-predecessor', 'S10-wl-structure',
                         'S11-deep-hierarchy', 'S12-transitive-chain']
        }
        with open(path / "metadata.json", 'w') as f:
            json.dump(metadata, f, indent=2)

        print(f"\nGenerated:")
        print(f"  - {len(self.materials)} materials")
        print(f"  - {len(self.bom_items)} BOM items")
        print(f"  - {len(self.ground_truth)} ground truth pairs")
        print(f"  - {len(self.goods_movements)} goods movements")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate synthetic BOM test data')
    parser.add_argument('--output', type=str, default='test/data/synthetic')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    generator = SyntheticBOMGenerator(seed=args.seed)
    generator.generate_all_scenarios()
    generator.save(args.output)

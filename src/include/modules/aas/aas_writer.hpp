#pragma once

#include "modules/aas/aas_tree_mapper.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Property trees and coverage results -> AAS v3 Part 1 JSON (GitHub #34)
//------------------------------------------------------------------------------
//
// The writer is the inverse of MapAasToTrees: a tree read from AAS is written back with the same
// idShort paths, so aas_to_trees(trees_to_aas(t)) gives t again (node ids, classes, values,
// bounds, counts, variant groups). Items whose attributes carry a role are written as IDTA-02020
// CapabilityContainers (Required / Offered); other items as SubmodelElementCollections with
// their properties. Every attribute key becomes a ConceptDescription (IEC 61360).

//! A capability catalogue entry, written as a ConceptDescription.
struct CatalogueEntry {
	std::string class_id;
	std::string label;
	std::string definition;
};

//! One environment (shells, submodels, ConceptDescriptions) for the trees in nodes.
std::string WriteTreesAsAas(const std::vector<TreeNodeRow> &nodes, const std::vector<CatalogueEntry> &catalogue);

//! A tree_coverage_trace row, grouped by requirement.
struct TraceAttrEntry {
	std::string attr_id, attr_name, required, offered, rule, name_rule, bound_source;
	bool has_score = false;
	double score = 0;
};

struct TraceRequirementEntry {
	std::string req_node_id, matched_node_id, matched_path, label_rule;
	double label_sim = 0;
	double req_score = 0;
	bool is_gap = false;
	double count_needed = 1;
	double count_available = 0;
	std::vector<TraceAttrEntry> attrs;
};

//! The explanation of one ranking as a Submodel ("CoverageResult", semanticIds in the
//! https://data-zoo.de/anofox/aas/ namespace until a standard template exists).
std::string WriteCoverageResult(const std::string &query_id, const std::string &candidate_id, double coverage,
                                const std::vector<TraceRequirementEntry> &requirements);

//! An idShort that satisfies AAS's pattern ^[a-zA-Z][a-zA-Z0-9_-]*[a-zA-Z0-9_]+$.
std::string AasIdShort(const std::string &text);

} // namespace anofox
} // namespace duckdb

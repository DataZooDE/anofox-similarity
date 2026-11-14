#pragma once

#include "modules/aas/aas_tree_mapper.hpp"

#include <limits>
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

//! A tree_coverage_trace row, grouped by requirement. Metrics are NaN when the trace had none:
//! they are written without a value.
struct TraceAttrEntry {
	std::string attr_id, attr_name, required, offered, rule, name_rule, bound_source;
	bool has_score = false;
	double score = std::numeric_limits<double>::quiet_NaN();
};

struct TraceRequirementEntry {
	std::string req_node_id, matched_node_id, matched_path, label_rule;
	double label_sim = std::numeric_limits<double>::quiet_NaN();
	double req_score = std::numeric_limits<double>::quiet_NaN();
	bool is_gap = false;
	double count_needed = std::numeric_limits<double>::quiet_NaN();
	double count_available = std::numeric_limits<double>::quiet_NaN();
	std::vector<TraceAttrEntry> attrs;
};

//! The explanation of one ranking as a Submodel ("CoverageResult", semanticIds in the
//! https://anofox.com/aas/ namespace until a standard template exists). Its id is id_prefix
//! (default https://anofox.com/aas/results/) plus the base64url-encoded query and candidate.
std::string WriteCoverageResult(const std::string &query_id, const std::string &candidate_id, double coverage,
                                const std::vector<TraceRequirementEntry> &requirements,
                                const std::string &id_prefix = std::string());

//! An idShort that satisfies AAS's pattern ^[a-zA-Z][a-zA-Z0-9_-]*[a-zA-Z0-9_]+$.
std::string AasIdShort(const std::string &text);

} // namespace anofox
} // namespace duckdb

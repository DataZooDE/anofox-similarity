#pragma once

#include "modules/aas/aas_model.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

class UnitTable;

//------------------------------------------------------------------------------
// AAS environment -> property trees (GitHub #27)
//------------------------------------------------------------------------------
//
// The rows mirror tree_nodes / tree_attributes (tree_from_json), so the SQL layer turns them
// into the same STRUCTs and the existing validate_trees -> tree_coverage pipeline applies.

struct TreeAttrRow {
	std::string attr_id;
	std::string name;
	std::string value_text;
	std::string dimension;
	std::string unit;
	bool has_value = false;
	double lo = 0; // base unit of the dimension; a one-sided value has lo = hi and a bound
	double hi = 0;
	std::string role;
	std::string bound;
	std::string bound_source; // where the bound came from: range, level_type, constraint, text
	std::string qualifier;
	std::string category;
	std::string attr_key; // the dictionary id as written (semanticId or supplementalSemanticId)
};

struct TreeNodeRow {
	std::string tree_id;
	std::string node_id;
	std::string parent_id;
	std::string node_kind; // container | item | variant_group
	std::string edge_type = "AND";
	std::string label;
	std::string context;
	std::string class_id;
	double count = 1;
	std::vector<TreeAttrRow> attrs;
	std::string submodel_id; // the AAS submodel the node comes from (empty for the shell's root)
};

//! A finding about the input, not an error: the mapping goes on.
struct AasIssue {
	std::string tree_id;
	std::string path; // idShort path, submodel idShort first
	std::string issue;
	std::string severity; // error | warning | info
	std::string detail;
	std::string submodel_id; // empty for findings about the whole input
};

//! A GeneralizedBy relation: class_id is a specialisation of parent_class_id.
struct ClassEdge {
	std::string class_id;
	std::string parent_class_id;
};

struct AasMapping {
	std::vector<TreeNodeRow> nodes;
	std::vector<AasIssue> issues;
	std::vector<ClassEdge> class_hierarchy;
};

struct AasMapOptions {
	std::string tree_id; // empty: the shell id (or submodel id for a shell-less submodel)
	const UnitTable *units = nullptr;
	std::vector<std::string> langs = {"en", "de"};
};

AasMapping MapAasToTrees(const AasEnvironment &env, const AasMapOptions &options);

//! "RatedCurrent" -> "Rated current", "IPRating" -> "IP rating", "max_speed" -> "Max speed".
std::string HumaniseIdShort(const std::string &id_short);

} // namespace anofox
} // namespace duckdb

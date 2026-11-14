#pragma once

#include "modules/aas/aas_model.hpp"
#include "modules/aas/aas_tree_mapper.hpp"

#include <string>
#include <vector>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// AAS data quality per submodel (GitHub #36): how much of a supplier's AAS the matching can use.
// It reads the reader's model and the mapper's issues; it does not run the coverage kernel.
//------------------------------------------------------------------------------

struct SubmodelQuality {
	std::string tree_id;
	std::string submodel_id;
	std::string submodel; // idShort
	std::string kind;     // Instance | Template
	size_t n_values = 0;            // Property, Range and MultiLanguageProperty elements
	size_t n_with_semantic_id = 0;  // ... with a dictionary semanticId (or supplementalSemanticId)
	size_t n_missing_concept_description = 0;
	size_t n_unresolved_units = 0;  // numbers whose unit is unknown
	size_t n_unparseable = 0;       // bad_value
	size_t n_mixed_releases = 0;    // concepts referenced in more than one dictionary release
	size_t n_issues = 0;            // all issues of the submodel
};

std::vector<SubmodelQuality> AssessAasQuality(const AasEnvironment &env, const AasMapping &mapping);

} // namespace anofox
} // namespace duckdb

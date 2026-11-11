#include "modules/aas/aas_quality.hpp"
#include "modules/aas/semantic_key.hpp"

#include <map>
#include <set>
#include <unordered_map>

namespace duckdb {
namespace anofox {

namespace {

bool IsStructural(const std::string &id) {
	return id.find("/idta/CapabilityPropertyType/") != std::string::npos ||
	       id.find("/idta/CapabilityDescription/") != std::string::npos;
}

std::string DictionaryId(const AasElement &e) {
	if (!e.semantic_id.empty() && !IsStructural(e.semantic_id)) {
		return e.semantic_id;
	}
	for (auto &s : e.supplemental_ids) {
		if (!IsStructural(s)) {
			return s;
		}
	}
	return std::string();
}

bool IsValue(const AasElement &e) {
	return e.model_type == "Property" || e.model_type == "Range" || e.model_type == "MultiLanguageProperty";
}

//! Every dictionary id of the submodel's value elements (with its release, as written). Members of
//! a SubmodelElementList take the list's id when they state none, as the reader does.
void Collect(const std::vector<AasElement> &elements, SubmodelQuality &q, std::vector<std::string> &ids,
             const std::string &inherited = std::string()) {
	for (auto &e : elements) {
		if (IsValue(e) && e.semantic_id.find("data-zoo.de/anofox/aas/Count") == std::string::npos) {
			q.n_values++;
			auto id = DictionaryId(e);
			if (id.empty()) {
				id = inherited;
			}
			if (!id.empty()) {
				q.n_with_semantic_id++;
				ids.push_back(id);
			}
		}
		Collect(e.children, q, ids, e.model_type == "SubmodelElementList" ? DictionaryId(e) : std::string());
	}
}

} // namespace

std::vector<SubmodelQuality> AssessAasQuality(const AasEnvironment &env, const AasMapping &mapping) {
	std::unordered_map<std::string, std::string> tree_of(mapping.submodel_trees.begin(), mapping.submodel_trees.end());
	// releases of each concept across the whole input
	std::map<std::string, std::set<std::string>> releases;
	std::vector<SubmodelQuality> out;
	std::vector<std::vector<std::string>> ids_of(env.submodels.size());
	for (size_t i = 0; i < env.submodels.size(); i++) {
		auto &sm = env.submodels[i];
		SubmodelQuality q;
		q.submodel_id = sm.id;
		q.submodel = sm.id_short.empty() ? sm.id : sm.id_short;
		q.kind = sm.kind;
		auto tree = tree_of.find(sm.id);
		q.tree_id = tree == tree_of.end() ? std::string() : tree->second;
		Collect(sm.elements, q, ids_of[i]);
		for (auto &id : ids_of[i]) {
			releases[SemanticKey(id).key].insert(SemanticKey(id, true).key);
		}
		out.push_back(q);
	}
	for (size_t i = 0; i < out.size(); i++) {
		auto &q = out[i];
		std::set<std::string> mixed;
		for (auto &id : ids_of[i]) {
			auto key = SemanticKey(id).key;
			if (releases[key].size() > 1) {
				mixed.insert(key);
			}
		}
		q.n_mixed_releases = mixed.size();
		// the mapper's issues of this submodel: same tree, path starting with its idShort
		for (auto &issue : mapping.issues) {
			auto &p = issue.path;
			bool here = issue.tree_id == q.tree_id && p.compare(0, q.submodel.size(), q.submodel) == 0 &&
			            (p.size() == q.submodel.size() || p[q.submodel.size()] == '.' || p[q.submodel.size()] == '[');
			if (!here) {
				continue;
			}
			q.n_issues++;
			if (issue.issue == "missing_concept_description" || issue.issue == "ambiguous_concept_description") {
				q.n_missing_concept_description++;
			}
			if (issue.issue == "missing_concept_description" || issue.issue == "ambiguous_concept_description" ||
			    issue.issue == "no_unit_information") {
				q.n_unresolved_units++;
			}
			if (issue.issue == "bad_value") {
				q.n_unparseable++;
			}
		}
		// a unit text the unit table does not know is unresolved too (dimension NULL with a value)
		for (auto &n : mapping.nodes) {
			if (n.tree_id != q.tree_id || n.node_id.compare(0, q.submodel.size(), q.submodel) != 0) {
				continue;
			}
			for (auto &a : n.attrs) {
				if (a.has_value && a.dimension.empty() && a.category.empty() && !a.unit.empty()) {
					q.n_unresolved_units++;
				}
			}
		}
	}
	return out;
}

} // namespace anofox
} // namespace duckdb

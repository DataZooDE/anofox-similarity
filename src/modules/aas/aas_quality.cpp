#include "modules/aas/aas_quality.hpp"
#include "modules/aas/semantic_key.hpp"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

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

//! The dictionary entries of the input: ConceptDescriptions by exact id and by concept (any release).
struct Dictionary {
	std::unordered_set<std::string> exact, concept;

	explicit Dictionary(const AasEnvironment &env) {
		for (auto &cd : env.concept_descriptions) {
			if (cd.has_data_spec) {
				exact.insert(SemanticKey(cd.id, true).key);
				concept.insert(SemanticKey(cd.id).key);
			}
		}
	}
	bool Knows(const std::string &id) const {
		return exact.count(SemanticKey(id, true).key) || concept.count(SemanticKey(id).key);
	}
};

//! Value elements of a submodel: counts of all, of the coded ones, and of coded ones without any
//! dictionary entry (no ConceptDescription for any of its ids, no embedded data specification).
//! Members of a SubmodelElementList take the list's id when they state none, as the reader does.
void Collect(const std::vector<AasElement> &elements, const Dictionary &dictionary, SubmodelQuality &q,
             std::vector<std::string> &ids, const AasElement *list = nullptr) {
	for (auto &e : elements) {
		if (IsValue(e) && e.semantic_id.find(ANOFOX_AAS_COUNT_ID) == std::string::npos) {
			q.n_values++;
			auto id = DictionaryId(e);
			if (id.empty() && list) {
				id = DictionaryId(*list);
			}
			if (!id.empty()) {
				q.n_with_semantic_id++;
				ids.push_back(id);
				bool embedded = e.has_data_spec || (list && list->has_data_spec);
				bool known = dictionary.Knows(id);
				for (auto &s : e.supplemental_ids) {
					known = known || dictionary.Knows(s);
				}
				if (!embedded && !known) {
					q.n_missing_concept_description++;
				}
			}
		}
		Collect(e.children, dictionary, q, ids, e.model_type == "SubmodelElementList" ? &e : nullptr);
	}
}

} // namespace

std::vector<SubmodelQuality> AssessAasQuality(const AasEnvironment &env, const AasMapping &mapping) {
	Dictionary dictionary(env);
	std::map<std::string, std::set<std::string>> releases; // concept -> releases in the whole input
	std::vector<SubmodelQuality> out;
	std::vector<std::vector<std::string>> ids_of(env.submodels.size());
	for (size_t i = 0; i < env.submodels.size(); i++) {
		auto &sm = env.submodels[i];
		SubmodelQuality q;
		q.submodel_id = sm.id;
		q.submodel = sm.id_short.empty() ? sm.id : sm.id_short;
		q.kind = sm.kind;
		Collect(sm.elements, dictionary, q, ids_of[i]);
		for (auto &id : ids_of[i]) {
			releases[SemanticKey(id).key].insert(SemanticKey(id, true).key);
		}
		out.push_back(q);
	}
	for (size_t i = 0; i < out.size(); i++) {
		auto &q = out[i];
		std::set<std::string> mixed;
		for (auto &id : ids_of[i]) {
			if (releases[SemanticKey(id).key].size() > 1) {
				mixed.insert(SemanticKey(id).key);
			}
		}
		q.n_mixed_releases = mixed.size();
		// the mapper tags every node and finding with its submodel id
		for (auto &n : mapping.nodes) {
			if (n.submodel_id != q.submodel_id) {
				continue;
			}
			q.tree_id = n.tree_id;
			for (auto &a : n.attrs) {
				if (a.has_value && a.dimension.empty() && a.category.empty()) {
					q.n_unresolved_units++; // a number whose unit is unknown, whatever the cause
				}
			}
		}
		for (auto &issue : mapping.issues) {
			if (issue.submodel_id != q.submodel_id) {
				continue;
			}
			if (issue.issue == "unmapped_text") {
				continue; // values without an id, counted by n_values - n_with_semantic_id already
			}
			q.n_issues++;
			if (issue.issue == "bad_value") {
				q.n_unparseable++;
			}
		}
	}
	return out;
}

} // namespace anofox
} // namespace duckdb

// AAS Part 1 JSON reader (IDTA-01001 v3.x serialisation) into the format-neutral AasEnvironment.
#include "modules/aas/aas_model.hpp"
#include "modules/property_tree.hpp" // MAX_TREE_DEPTH

#include "duckdb/common/exception.hpp"

#include <nlohmann/json.hpp>

namespace duckdb {
namespace anofox {

namespace {

using Json = nlohmann::json;

std::string Str(const Json &j, const char *key) {
	auto it = j.find(key);
	if (it == j.end() || it->is_null()) {
		return std::string();
	}
	if (it->is_string()) {
		return it->get<std::string>();
	}
	if (it->is_boolean()) {
		return it->get<bool>() ? "true" : "false";
	}
	if (it->is_number()) {
		return it->dump(); // values are strings in v3; tolerate numbers from v2-style exports
	}
	return std::string();
}

bool Has(const Json &j, const char *key) {
	auto it = j.find(key);
	return it != j.end() && !it->is_null();
}

AasReference Ref(const Json &j) {
	AasReference r;
	if (!j.is_object()) {
		return r;
	}
	r.type = Str(j, "type");
	auto keys = j.find("keys");
	if (keys != j.end() && keys->is_array()) {
		for (auto &k : *keys) {
			if (k.is_object()) {
				r.keys.emplace_back(Str(k, "type"), Str(k, "value"));
			}
		}
	}
	return r;
}

std::string RefValue(const Json &j, const char *key) {
	auto it = j.find(key);
	return it == j.end() ? std::string() : Ref(*it).Last();
}

//! English text of a language-string list, else the first entry.
std::string LangText(const Json &j, const char *key) {
	auto it = j.find(key);
	if (it == j.end() || !it->is_array() || it->empty()) {
		return std::string();
	}
	for (auto &e : *it) {
		auto lang = Str(e, "language");
		if (lang == "en" || lang.rfind("en-", 0) == 0) {
			return Str(e, "text");
		}
	}
	return Str((*it)[0], "text");
}

bool LevelFlag(const Json &level, const char *key) {
	auto it = level.find(key);
	if (it == level.end()) {
		return false;
	}
	if (it->is_boolean()) {
		return it->get<bool>();
	}
	return it->is_string() && (it->get<std::string>() == "true" || it->get<std::string>() == "1");
}

bool ReadIec61360(const Json &j, AasIec61360 &out) {
	auto specs = j.find("embeddedDataSpecifications");
	if (specs == j.end() || !specs->is_array()) {
		return false;
	}
	for (auto &spec : *specs) {
		auto content = spec.find("dataSpecificationContent");
		if (content == spec.end() || !content->is_object()) {
			continue;
		}
		auto model = Str(*content, "modelType");
		auto template_id = RefValue(spec, "dataSpecification");
		bool iec = model.empty() ? template_id.find("IEC61360") != std::string::npos ||
		                               template_id.find("Iec61360") != std::string::npos ||
		                               Has(*content, "preferredName")
		                         : model == "DataSpecificationIec61360";
		if (!iec) {
			continue;
		}
		out.preferred_name = LangText(*content, "preferredName");
		out.definition = LangText(*content, "definition");
		out.unit = Str(*content, "unit");
		out.unit_id = RefValue(*content, "unitId");
		out.data_type = Str(*content, "dataType");
		auto level = content->find("levelType");
		if (level != content->end() && level->is_object()) {
			out.has_level_type = true;
			out.level_min = LevelFlag(*level, "min");
			out.level_nom = LevelFlag(*level, "nom");
			out.level_typ = LevelFlag(*level, "typ");
			out.level_max = LevelFlag(*level, "max");
		}
		return true;
	}
	return false;
}

std::vector<AasQualifier> Qualifiers(const Json &j) {
	std::vector<AasQualifier> out;
	auto it = j.find("qualifiers");
	if (it == j.end() || !it->is_array()) {
		return out;
	}
	for (auto &q : *it) {
		out.push_back({Str(q, "type"), Str(q, "valueType"), Str(q, "value"), RefValue(q, "semanticId")});
	}
	return out;
}

AasElement Element(const Json &j, size_t depth);

void Children(const Json &list, AasElement &parent, size_t depth) {
	if (!list.is_array()) {
		return;
	}
	for (auto &c : list) {
		if (c.is_object()) {
			parent.children.push_back(Element(c, depth + 1));
		}
	}
}

AasElement Element(const Json &j, size_t depth) {
	if (depth > MAX_TREE_DEPTH) {
		throw InvalidInputException("AAS: submodel elements are nested deeper than %llu levels",
		                            (unsigned long long)MAX_TREE_DEPTH);
	}
	AasElement e;
	e.model_type = Str(j, "modelType");
	e.id_short = Str(j, "idShort");
	e.display_name = LangText(j, "displayName");
	e.semantic_id = RefValue(j, "semanticId");
	auto sup = j.find("supplementalSemanticIds");
	if (sup != j.end() && sup->is_array()) {
		for (auto &r : *sup) {
			auto v = Ref(r).Last();
			if (!v.empty()) {
				e.supplemental_ids.push_back(v);
			}
		}
	}
	e.qualifiers = Qualifiers(j);
	e.value_type = Str(j, "valueType");
	e.has_data_spec = ReadIec61360(j, e.data_spec);
	if (e.model_type == "SubmodelElementCollection" || e.model_type == "SubmodelElementList") {
		auto v = j.find("value");
		if (v != j.end()) {
			Children(*v, e, depth);
		}
		e.semantic_id_list_element = RefValue(j, "semanticIdListElement");
		e.value_type_list_element = Str(j, "valueTypeListElement");
		AasInheritListElement(e);
	} else if (e.model_type == "Entity") {
		auto v = j.find("statements");
		if (v != j.end()) {
			Children(*v, e, depth);
		}
	} else if (e.model_type == "MultiLanguageProperty") {
		auto v = j.find("value");
		if (v != j.end() && v->is_array()) {
			for (auto &ls : *v) {
				e.lang_values.emplace_back(Str(ls, "language"), Str(ls, "text"));
			}
			e.has_value = !e.lang_values.empty();
		}
	} else if (e.model_type == "Range") {
		e.has_min = Has(j, "min");
		e.min = Str(j, "min");
		e.has_max = Has(j, "max");
		e.max = Str(j, "max");
	} else if (e.model_type == "RelationshipElement" || e.model_type == "AnnotatedRelationshipElement") {
		auto f = j.find("first");
		if (f != j.end()) {
			e.first = Ref(*f);
		}
		auto s = j.find("second");
		if (s != j.end()) {
			e.second = Ref(*s);
		}
	} else {
		auto v = j.find("value");
		if (v != j.end() && !v->is_null() && !v->is_object() && !v->is_array()) {
			e.has_value = true;
			e.value = Str(j, "value");
		}
		e.value_id = RefValue(j, "valueId");
	}
	return e;
}

AasSubmodel Submodel(const Json &j) {
	AasSubmodel sm;
	sm.id = Str(j, "id");
	sm.id_short = Str(j, "idShort");
	sm.semantic_id = RefValue(j, "semanticId");
	sm.kind = Str(j, "kind");
	sm.qualifiers = Qualifiers(j);
	auto els = j.find("submodelElements");
	if (els != j.end() && els->is_array()) {
		for (auto &e : *els) {
			if (e.is_object()) {
				sm.elements.push_back(Element(e, 0));
			}
		}
	}
	return sm;
}

AasShell Shell(const Json &j) {
	AasShell s;
	s.id = Str(j, "id");
	s.id_short = Str(j, "idShort");
	auto info = j.find("assetInformation");
	if (info != j.end() && info->is_object()) {
		s.global_asset_id = Str(*info, "globalAssetId");
	}
	auto sms = j.find("submodels");
	if (sms != j.end() && sms->is_array()) {
		for (auto &r : *sms) {
			auto v = Ref(r).Last();
			if (!v.empty()) {
				s.submodel_ids.push_back(v);
			}
		}
	}
	return s;
}

AasConceptDescription ConceptDescription(const Json &j) {
	AasConceptDescription cd;
	cd.id = Str(j, "id");
	cd.id_short = Str(j, "idShort");
	cd.has_data_spec = ReadIec61360(j, cd.data_spec);
	return cd;
}

void ReadArray(const Json &doc, const char *key, AasEnvironment &env) {
	auto it = doc.find(key);
	if (it == doc.end() || it->is_null()) {
		return;
	}
	if (!it->is_array()) {
		env.notes.push_back({"malformed_member", "'" + std::string(key) + "' must be a list"});
		return;
	}
	for (auto &x : *it) {
		if (!x.is_object()) {
			env.notes.push_back({"malformed_member", "an entry of '" + std::string(key) + "' is not an object"});
			continue;
		}
		if (std::string(key) == "assetAdministrationShells") {
			env.shells.push_back(Shell(x));
		} else if (std::string(key) == "submodels") {
			env.submodels.push_back(Submodel(x));
		} else {
			env.concept_descriptions.push_back(ConceptDescription(x));
		}
	}
}

void ReadTopLevel(const Json &doc, AasEnvironment &env) {
	auto model = Str(doc, "modelType");
	if (model == "Submodel" || (model.empty() && Has(doc, "submodelElements"))) {
		env.submodels.push_back(Submodel(doc));
	} else if (model == "AssetAdministrationShell" || (model.empty() && Has(doc, "assetInformation"))) {
		env.shells.push_back(Shell(doc));
	} else if (model == "ConceptDescription") {
		env.concept_descriptions.push_back(ConceptDescription(doc));
	} else if (Has(doc, "result") && doc["result"].is_array()) {
		for (auto &x : doc["result"]) { // Part 2 paged result: {"result": [...], "paging_metadata": {...}}
			if (x.is_object()) {
				ReadTopLevel(x, env);
			}
		}
	} else {
		ReadArray(doc, "assetAdministrationShells", env);
		ReadArray(doc, "submodels", env);
		ReadArray(doc, "conceptDescriptions", env);
	}
}

} // namespace

AasEnvironment ReadAasJson(const std::string &text) {
	Json doc;
	try {
		doc = Json::parse(text);
	} catch (const Json::parse_error &e) {
		throw InvalidInputException("AAS JSON: %s", e.what());
	}
	AasEnvironment env;
	if (doc.is_array()) {
		for (auto &x : doc) {
			if (x.is_object()) {
				ReadTopLevel(x, env);
			}
		}
	} else if (doc.is_object()) {
		ReadTopLevel(doc, env);
	} else {
		throw InvalidInputException("AAS JSON: expected an object (Environment, Submodel or paged result)");
	}
	return env;
}

} // namespace anofox
} // namespace duckdb

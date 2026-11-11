// AAS Part 1 XML reader (IDTA-01001 v3.x schema, namespaces https://admin-shell.io/aas/3/0 and /3/1)
// into the format-neutral AasEnvironment. tinyxml2 is the XML library erpl-odata uses as well.
//
// Elements are matched by local name, so a prefixed document (<aas:environment>) reads the
// same as a default-namespace one.
#include "modules/aas/aas_model.hpp"
#include "modules/property_tree.hpp" // MAX_TREE_DEPTH

#include "duckdb/common/exception.hpp"

#include <tinyxml2.h>

#include <cctype>
#include <cstring>

namespace duckdb {
namespace anofox {

namespace {

using tinyxml2::XMLElement;

const char *LocalName(const XMLElement *e) {
	auto name = e->Name();
	auto colon = std::strrchr(name, ':');
	return colon ? colon + 1 : name;
}

bool Is(const XMLElement *e, const char *local) {
	return std::strcmp(LocalName(e), local) == 0;
}

const XMLElement *Child(const XMLElement *e, const char *local) {
	if (!e) {
		return nullptr;
	}
	for (auto c = e->FirstChildElement(); c; c = c->NextSiblingElement()) {
		if (Is(c, local)) {
			return c;
		}
	}
	return nullptr;
}

std::string Text(const XMLElement *e) {
	if (!e) {
		return std::string();
	}
	auto t = e->GetText();
	return t ? std::string(t) : std::string();
}

std::string ChildText(const XMLElement *e, const char *local) {
	return Text(Child(e, local));
}

AasReference Ref(const XMLElement *e) {
	AasReference r;
	if (!e) {
		return r;
	}
	r.type = ChildText(e, "type");
	auto keys = Child(e, "keys");
	if (keys) {
		for (auto k = keys->FirstChildElement(); k; k = k->NextSiblingElement()) {
			if (Is(k, "key")) {
				r.keys.emplace_back(ChildText(k, "type"), ChildText(k, "value"));
			}
		}
	}
	return r;
}

std::string RefValue(const XMLElement *e, const char *local) {
	return Ref(Child(e, local)).Last();
}

//! English text of a language-string list (<langStringNameType>...), else the first entry.
std::string LangText(const XMLElement *list) {
	if (!list) {
		return std::string();
	}
	std::string first;
	for (auto ls = list->FirstChildElement(); ls; ls = ls->NextSiblingElement()) {
		auto lang = ChildText(ls, "language");
		auto text = ChildText(ls, "text");
		if (lang == "en" || lang.rfind("en-", 0) == 0) {
			return text;
		}
		if (first.empty()) {
			first = text;
		}
	}
	return first;
}

bool Flag(const XMLElement *level, const char *local) {
	auto t = ChildText(level, local);
	return t == "true" || t == "1";
}

bool ReadIec61360(const XMLElement *e, AasIec61360 &out) {
	auto specs = Child(e, "embeddedDataSpecifications");
	if (!specs) {
		return false;
	}
	for (auto spec = specs->FirstChildElement(); spec; spec = spec->NextSiblingElement()) {
		auto content = Child(Child(spec, "dataSpecificationContent"), "dataSpecificationIec61360");
		if (!content) {
			continue;
		}
		out.preferred_name = LangText(Child(content, "preferredName"));
		out.definition = LangText(Child(content, "definition"));
		out.unit = ChildText(content, "unit");
		out.unit_id = RefValue(content, "unitId");
		out.data_type = ChildText(content, "dataType");
		auto level = Child(content, "levelType");
		if (level) {
			out.has_level_type = true;
			out.level_min = Flag(level, "min");
			out.level_nom = Flag(level, "nom");
			out.level_typ = Flag(level, "typ");
			out.level_max = Flag(level, "max");
		}
		return true;
	}
	return false;
}

std::vector<AasQualifier> Qualifiers(const XMLElement *e) {
	std::vector<AasQualifier> out;
	auto list = Child(e, "qualifiers");
	if (!list) {
		return out;
	}
	for (auto q = list->FirstChildElement(); q; q = q->NextSiblingElement()) {
		out.push_back(
		    {ChildText(q, "type"), ChildText(q, "valueType"), ChildText(q, "value"), RefValue(q, "semanticId")});
	}
	return out;
}

//! XML element name -> metamodel class name (the JSON modelType).
std::string ModelType(const XMLElement *e) {
	std::string local = LocalName(e);
	if (local.empty()) {
		return local;
	}
	local[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(local[0])));
	return local;
}

AasElement Element(const XMLElement *x, size_t depth);

void Children(const XMLElement *list, AasElement &parent, size_t depth) {
	if (!list) {
		return;
	}
	for (auto c = list->FirstChildElement(); c; c = c->NextSiblingElement()) {
		parent.children.push_back(Element(c, depth + 1));
	}
}

AasElement Element(const XMLElement *x, size_t depth) {
	if (depth > MAX_TREE_DEPTH) {
		throw InvalidInputException("AAS: submodel elements are nested deeper than %llu levels",
		                            (unsigned long long)MAX_TREE_DEPTH);
	}
	AasElement e;
	e.model_type = ModelType(x);
	e.id_short = ChildText(x, "idShort");
	e.display_name = LangText(Child(x, "displayName"));
	e.semantic_id = RefValue(x, "semanticId");
	auto sup = Child(x, "supplementalSemanticIds");
	if (sup) {
		for (auto r = sup->FirstChildElement(); r; r = r->NextSiblingElement()) {
			auto v = Ref(r).Last();
			if (!v.empty()) {
				e.supplemental_ids.push_back(v);
			}
		}
	}
	e.qualifiers = Qualifiers(x);
	e.value_type = ChildText(x, "valueType");
	e.has_data_spec = ReadIec61360(x, e.data_spec);
	if (e.model_type == "SubmodelElementCollection" || e.model_type == "SubmodelElementList") {
		Children(Child(x, "value"), e, depth);
		if (e.value_type.empty()) {
			e.value_type = ChildText(x, "valueTypeListElement");
		}
	} else if (e.model_type == "Entity") {
		Children(Child(x, "statements"), e, depth);
	} else if (e.model_type == "MultiLanguageProperty") {
		auto v = Child(x, "value");
		if (v) {
			for (auto ls = v->FirstChildElement(); ls; ls = ls->NextSiblingElement()) {
				e.lang_values.emplace_back(ChildText(ls, "language"), ChildText(ls, "text"));
			}
		}
		e.has_value = !e.lang_values.empty();
	} else if (e.model_type == "Range") {
		auto mn = Child(x, "min");
		auto mx = Child(x, "max");
		e.has_min = mn != nullptr;
		e.min = Text(mn);
		e.has_max = mx != nullptr;
		e.max = Text(mx);
	} else if (e.model_type == "RelationshipElement" || e.model_type == "AnnotatedRelationshipElement") {
		e.first = Ref(Child(x, "first"));
		e.second = Ref(Child(x, "second"));
	} else {
		auto v = Child(x, "value");
		if (v && !v->FirstChildElement()) {
			e.has_value = true;
			e.value = Text(v);
		}
		e.value_id = RefValue(x, "valueId");
	}
	return e;
}

AasSubmodel Submodel(const XMLElement *x) {
	AasSubmodel sm;
	sm.id = ChildText(x, "id");
	sm.id_short = ChildText(x, "idShort");
	sm.semantic_id = RefValue(x, "semanticId");
	sm.kind = ChildText(x, "kind");
	sm.qualifiers = Qualifiers(x);
	auto els = Child(x, "submodelElements");
	if (els) {
		for (auto e = els->FirstChildElement(); e; e = e->NextSiblingElement()) {
			sm.elements.push_back(Element(e, 0));
		}
	}
	return sm;
}

AasShell Shell(const XMLElement *x) {
	AasShell s;
	s.id = ChildText(x, "id");
	s.id_short = ChildText(x, "idShort");
	s.global_asset_id = ChildText(Child(x, "assetInformation"), "globalAssetId");
	auto sms = Child(x, "submodels");
	if (sms) {
		for (auto r = sms->FirstChildElement(); r; r = r->NextSiblingElement()) {
			auto v = Ref(r).Last();
			if (!v.empty()) {
				s.submodel_ids.push_back(v);
			}
		}
	}
	return s;
}

AasConceptDescription ConceptDescription(const XMLElement *x) {
	AasConceptDescription cd;
	cd.id = ChildText(x, "id");
	cd.id_short = ChildText(x, "idShort");
	cd.has_data_spec = ReadIec61360(x, cd.data_spec);
	return cd;
}

void ReadList(const XMLElement *root, const char *list, const char *item, AasEnvironment &env) {
	auto l = Child(root, list);
	if (!l) {
		return;
	}
	for (auto x = l->FirstChildElement(); x; x = x->NextSiblingElement()) {
		if (!Is(x, item)) {
			continue;
		}
		if (std::strcmp(item, "assetAdministrationShell") == 0) {
			env.shells.push_back(Shell(x));
		} else if (std::strcmp(item, "submodel") == 0) {
			env.submodels.push_back(Submodel(x));
		} else {
			env.concept_descriptions.push_back(ConceptDescription(x));
		}
	}
}

} // namespace

AasEnvironment ReadAasXml(const std::string &text) {
	tinyxml2::XMLDocument doc;
	if (doc.Parse(text.data(), text.size()) != tinyxml2::XML_SUCCESS) {
		throw InvalidInputException("AAS XML: %s (line %d)", doc.ErrorStr() ? doc.ErrorStr() : "parse error",
		                            doc.ErrorLineNum());
	}
	auto root = doc.RootElement();
	AasEnvironment env;
	if (!root) {
		throw InvalidInputException("AAS XML: the document has no root element");
	}
	if (Is(root, "environment")) {
		ReadList(root, "assetAdministrationShells", "assetAdministrationShell", env);
		ReadList(root, "submodels", "submodel", env);
		ReadList(root, "conceptDescriptions", "conceptDescription", env);
	} else if (Is(root, "submodel")) {
		env.submodels.push_back(Submodel(root));
	} else {
		throw InvalidInputException("AAS XML: expected <environment> or <submodel>, got <%s>", root->Name());
	}
	return env;
}

} // namespace anofox
} // namespace duckdb

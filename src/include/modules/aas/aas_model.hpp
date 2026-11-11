#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// A format-neutral, read-only view of an AAS v3.x environment (IDTA-01001 Part 1).
//------------------------------------------------------------------------------
//
// The JSON, XML and AASX readers fill it. The tree mapper and the quality report read it.
// It keeps only what matching needs: identifiers, values, units and the structure. Display
// texts are kept in one language (English preferred, else the first entry).

//! A Reference: its keys in order. The identifier it names is the value of the last key.
struct AasReference {
	std::string type; // ExternalReference | ModelReference
	std::vector<std::pair<std::string, std::string>> keys; // (key type, value)

	bool Empty() const {
		return keys.empty();
	}
	std::string Last() const {
		return keys.empty() ? std::string() : keys.back().second;
	}
};

struct AasQualifier {
	std::string type;
	std::string value_type;
	std::string value;
	std::string semantic_id;
};

//! IEC 61360 data specification content (IDTA-01003-a): the dictionary entry of a property.
struct AasIec61360 {
	std::string preferred_name;
	std::string definition;
	std::string unit;    // symbol text as written
	std::string unit_id; // the last key of the unitId reference (ECLASS unit IRDI, Rec 20 code ...)
	std::string data_type;
	bool has_level_type = false;
	bool level_min = false;
	bool level_nom = false;
	bool level_typ = false;
	bool level_max = false;
};

//! One SubmodelElement. Containers (SubmodelElementCollection, SubmodelElementList, Entity)
//! keep their children; leaves keep their value as written (strings typed by value_type).
struct AasElement {
	std::string model_type; // Property, Range, SubmodelElementCollection, Capability, ...
	std::string id_short;   // empty for children of a SubmodelElementList
	std::string display_name;
	std::string semantic_id;
	std::vector<std::string> supplemental_ids;
	std::vector<AasQualifier> qualifiers;

	std::string value_type; // xs:double, xs:string, ...
	bool has_value = false;
	std::string value;      // Property
	std::vector<std::pair<std::string, std::string>> lang_values; // MultiLanguageProperty: (language, text)
	bool has_min = false;
	std::string min; // Range
	bool has_max = false;
	std::string max;
	std::string value_id; // Property: enumeration value identifier

	bool has_data_spec = false;
	AasIec61360 data_spec; // embedded on the element itself (IDTA-02020 hasDataSpecification)

	AasReference first; // RelationshipElement
	AasReference second;

	std::vector<AasElement> children;
};

struct AasSubmodel {
	std::string id;
	std::string id_short;
	std::string semantic_id;
	std::string kind; // Instance | Template
	std::vector<AasQualifier> qualifiers;
	std::vector<AasElement> elements;
};

struct AasShell {
	std::string id;
	std::string id_short;
	std::string global_asset_id;
	std::vector<std::string> submodel_ids; // from the ModelReferences in 'submodels'
};

struct AasConceptDescription {
	std::string id;
	std::string id_short;
	bool has_data_spec = false;
	AasIec61360 data_spec;
};

struct AasEnvironment {
	std::vector<AasShell> shells;
	std::vector<AasSubmodel> submodels;
	std::vector<AasConceptDescription> concept_descriptions;

	void Append(AasEnvironment &&other);
};

//! Model types that hold other elements.
bool AasIsContainer(const std::string &model_type);

//------------------------------------------------------------------------------
// Readers. Each throws InvalidInputException with a readable message on malformed input.
//------------------------------------------------------------------------------

//! Part 1 JSON: an Environment, a single Submodel or Shell, or a Part 2 paged result
//! ({"result": [...]}) of submodels.
AasEnvironment ReadAasJson(const std::string &text);
//! Part 1 XML: an <environment> (namespaces 3.0 and 3.1, any prefix) or a single <submodel>.
AasEnvironment ReadAasXml(const std::string &text);
//! An AASX package (OPC zip): follows _rels/.rels -> aasx-origin -> aas-spec parts (XML or JSON).
AasEnvironment ReadAasx(const std::string &bytes);
//! Detects the format from the first bytes (PK zip, '<' XML, '{' or '[' JSON) and reads it.
AasEnvironment ReadAas(const std::string &content);

} // namespace anofox
} // namespace duckdb

// Shared parts of the AAS model: environment merging, list-element inheritance and the format
// dispatch of ReadAas.
#include "modules/aas/aas_model.hpp"

#include "duckdb/common/exception.hpp"

#include <cctype>

namespace duckdb {
namespace anofox {

void AasEnvironment::Append(AasEnvironment &&other) {
	for (auto &s : other.shells) {
		shells.push_back(std::move(s));
	}
	for (auto &s : other.submodels) {
		submodels.push_back(std::move(s));
	}
	for (auto &c : other.concept_descriptions) {
		concept_descriptions.push_back(std::move(c));
	}
	for (auto &n : other.notes) {
		notes.push_back(std::move(n));
	}
}

bool AasIsContainer(const std::string &model_type) {
	return model_type == "SubmodelElementCollection" || model_type == "SubmodelElementList" || model_type == "Entity";
}

void AasInheritListElement(AasElement &list) {
	if (list.model_type != "SubmodelElementList") {
		return;
	}
	for (auto &c : list.children) {
		if (c.semantic_id.empty()) {
			c.semantic_id = list.semantic_id_list_element;
		}
		if (c.value_type.empty()) {
			c.value_type = list.value_type_list_element;
		}
	}
}

AasEnvironment ReadAas(const std::string &content) {
	size_t i = 0;
	if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
	    static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
		i = 3; // UTF-8 byte order mark
	}
	if (content.size() >= 4 && content.compare(0, 4, "PK\x03\x04") == 0) {
		return ReadAasx(content);
	}
	while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
		i++;
	}
	if (i == content.size()) {
		throw InvalidInputException("AAS: empty input");
	}
	if (content[i] == '<') {
		return ReadAasXml(content);
	}
	if (content[i] == '{' || content[i] == '[') {
		return ReadAasJson(content.substr(i));
	}
	throw InvalidInputException("AAS: expected JSON, XML or an AASX package (zip)");
}

} // namespace anofox
} // namespace duckdb

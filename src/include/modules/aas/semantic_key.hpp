#pragma once

#include <string>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Identifier normalisation for AAS semanticIds (GitHub #29)
//------------------------------------------------------------------------------
//
// The same dictionary concept arrives in several spellings:
//   IRDI        0173-1#02-AAC895#008             (ECLASS, ISO 29002-5)
//   ECLASS IRI  https://api.eclass-cdp.com/0173-1-02-AAC895-008
//   IEC CDD     0112/2///61360_4#AAA001#001
// SemanticKey maps them onto one canonical key: the upper-cased IRDI without the version
// suffix (with it when keep_version). Other URIs pass through with scheme and host
// lower-cased; anything else is trimmed and returned unchanged. A blank input has no key.

struct SemanticKeyResult {
	bool has_key = false;
	std::string key;
};

SemanticKeyResult SemanticKey(const std::string &id, bool keep_version = false);

} // namespace anofox
} // namespace duckdb

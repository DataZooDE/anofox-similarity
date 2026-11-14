#include "modules/aas/semantic_key.hpp"

#include <cctype>
#include <vector>

namespace duckdb {
namespace anofox {

namespace {

bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

bool IsAlnum(char c) {
	return std::isalnum(static_cast<unsigned char>(c)) != 0;
}

bool AllDigits(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (!IsDigit(c)) {
			return false;
		}
	}
	return true;
}

std::string Upper(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::string Trim(const std::string &s) {
	size_t b = 0;
	size_t e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string Irdi(const std::string &rai, const std::string &item, const std::string &version, bool keep_version) {
	auto key = Upper(rai) + "#" + Upper(item);
	if (keep_version && !version.empty()) {
		key += "#" + version;
	}
	return key;
}

//! IRDI: <RAI>#<item>[#<version>] where the RAI starts with four digits and '-' or '/'
//! (0173-1, 0173-1---BASIC_1_1, 0112/2///61360_4), the item is alphanumeric with '-' or '_'
//! (02-AAC895, AAA001) and the version is all digits.
bool ParseIrdi(const std::string &s, bool keep_version, std::string &out) {
	if (s.size() < 6 || !AllDigits(s.substr(0, 4)) || (s[4] != '-' && s[4] != '/')) {
		return false;
	}
	auto first = s.find('#');
	if (first == std::string::npos || first < 5) {
		return false;
	}
	auto rai = s.substr(0, first);
	for (char c : rai) {
		if (!IsAlnum(c) && c != '-' && c != '/' && c != '_' && c != '.') {
			return false;
		}
	}
	auto rest = s.substr(first + 1);
	auto second = rest.find('#');
	auto item = second == std::string::npos ? rest : rest.substr(0, second);
	auto version = second == std::string::npos ? std::string() : rest.substr(second + 1);
	if (item.empty() || (second != std::string::npos && !AllDigits(version))) {
		return false;
	}
	for (char c : item) {
		if (!IsAlnum(c) && c != '-' && c != '_') {
			return false;
		}
	}
	out = Irdi(rai, item, version, keep_version);
	return true;
}

//! A compound IRDI, e.g. property-in-class: 0173-1#02-ABK161#002/0173-1#01-AHX838#002. Parts are
//! split at a '/' that ends a part with an item code and starts a new RAI (four digits, then '-'
//! or '/'); the slashes inside an IEC CDD RAI (0112/2///62683) never match.
bool ParseCompoundIrdi(const std::string &s, bool keep_version, std::string &out) {
	std::string result;
	size_t start = 0;
	for (size_t i = 0; i < s.size(); i++) {
		bool boundary = s[i] == '/' && i + 5 < s.size() && AllDigits(s.substr(i + 1, 4)) &&
		                (s[i + 5] == '-' || s[i + 5] == '/') && s.substr(start, i - start).find('#') != std::string::npos;
		if (!boundary) {
			continue;
		}
		std::string part;
		if (!ParseIrdi(s.substr(start, i - start), keep_version, part)) {
			return false;
		}
		result += part + "/";
		start = i + 1;
	}
	if (start == 0) {
		return false; // not compound
	}
	std::string last;
	if (!ParseIrdi(s.substr(start), keep_version, last)) {
		return false;
	}
	out = result + last;
	return true;
}

//! ECLASS IRI: an eclass host whose last path segment is the IRDI with '-' for '#', read from the
//! right: [version] (digits), item code, code space (two digits), and the registration authority
//! with the rest (it may contain dashes: 0173-1---BASIC_1_1).
//!   0173-1-02-AAC895-008            -> 0173-1#02-AAC895#008
//!   0173-1-02-AAC895                -> 0173-1#02-AAC895
//!   0173-1---BASIC_1_1-01-AAN431-007 -> 0173-1---BASIC_1_1#01-AAN431#007
bool ParseEclassIri(const std::string &host, const std::string &path, bool keep_version, std::string &out) {
	if (Lower(host).find("eclass") == std::string::npos) {
		return false;
	}
	auto end = path.find_first_of("?#");
	auto clean = path.substr(0, end);
	while (!clean.empty() && clean.back() == '/') {
		clean.pop_back();
	}
	auto slash = clean.rfind('/');
	auto seg = slash == std::string::npos ? clean : clean.substr(slash + 1);
	std::vector<std::string> parts;
	size_t start = 0;
	for (size_t i = 0; i <= seg.size(); i++) {
		if (i == seg.size() || seg[i] == '-') {
			parts.push_back(seg.substr(start, i - start));
			start = i + 1;
		}
	}
	std::string version;
	if (parts.size() >= 4 && AllDigits(parts.back()) && !AllDigits(parts[parts.size() - 2])) {
		version = parts.back();
		parts.pop_back();
	}
	if (parts.size() < 3) {
		return false;
	}
	auto item = parts.back();
	parts.pop_back();
	auto code_space = parts.back();
	parts.pop_back();
	if (code_space.size() != 2 || !AllDigits(code_space) || item.empty()) {
		return false;
	}
	std::string rai;
	for (auto &p : parts) {
		rai += (rai.empty() ? "" : "-") + p;
	}
	auto irdi = rai + "#" + code_space + "-" + item + (version.empty() ? "" : "#" + version);
	return ParseIrdi(irdi, keep_version, out);
}

//! scheme://host/path or scheme:rest. Returns false when s is not a URI.
bool SplitUri(const std::string &s, std::string &scheme, std::string &host, std::string &path, bool &authority) {
	auto colon = s.find(':');
	if (colon == std::string::npos || colon == 0) {
		return false;
	}
	if (!std::isalpha(static_cast<unsigned char>(s[0]))) {
		return false;
	}
	for (size_t i = 1; i < colon; i++) {
		char c = s[i];
		if (!IsAlnum(c) && c != '+' && c != '-' && c != '.') {
			return false;
		}
	}
	if (s.find(' ') != std::string::npos) {
		return false;
	}
	scheme = s.substr(0, colon);
	auto rest = s.substr(colon + 1);
	authority = rest.compare(0, 2, "//") == 0;
	if (authority) {
		auto h_end = rest.find_first_of("/?#", 2);
		host = rest.substr(2, h_end == std::string::npos ? std::string::npos : h_end - 2);
		path = h_end == std::string::npos ? std::string() : rest.substr(h_end);
	} else {
		// Without an authority only URNs count; "Max:5" is a value, not an identifier.
		if (Lower(scheme) != "urn") {
			return false;
		}
		host.clear();
		path = rest;
	}
	return true;
}

} // namespace

SemanticKeyResult SemanticKey(const std::string &id, bool keep_version) {
	SemanticKeyResult result;
	auto s = Trim(id);
	if (s.empty()) {
		return result;
	}
	result.has_key = true;
	if (ParseIrdi(s, keep_version, result.key) || ParseCompoundIrdi(s, keep_version, result.key)) {
		return result;
	}
	std::string scheme, host, path;
	bool authority = false;
	if (SplitUri(s, scheme, host, path, authority)) {
		if (authority && ParseEclassIri(host, path, keep_version, result.key)) {
			return result;
		}
		result.key = Lower(scheme) + ":" + (authority ? "//" + Lower(host) : std::string()) + path;
		return result;
	}
	result.key = s;
	return result;
}

} // namespace anofox
} // namespace duckdb

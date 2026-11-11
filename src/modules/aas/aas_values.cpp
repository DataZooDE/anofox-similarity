#include "modules/aas/aas_values.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace duckdb {
namespace anofox {

std::string AasTrim(const std::string &s) {
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
		b++;
	}
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
		e--;
	}
	return s.substr(b, e - b);
}

std::string AasLower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

bool IsIntegerXsdType(const std::string &value_type) {
	static const std::unordered_set<std::string> INTEGER {
	    "xs:integer",          "xs:int",          "xs:long",           "xs:short",
	    "xs:byte",             "xs:unsignedint",  "xs:unsignedlong",   "xs:unsignedshort",
	    "xs:unsignedbyte",     "xs:nonnegativeinteger", "xs:positiveinteger", "xs:negativeinteger",
	    "xs:nonpositiveinteger"};
	return INTEGER.count(AasLower(value_type)) > 0;
}

bool IsNumericXsdType(const std::string &value_type) {
	auto t = AasLower(value_type);
	return t == "xs:double" || t == "xs:float" || t == "xs:decimal" || IsIntegerXsdType(t);
}

namespace {

//! The whole text is one finite number in C syntax.
bool StrictNumber(const std::string &t, double &out) {
	if (t.empty()) {
		return false;
	}
	errno = 0;
	char *end = nullptr;
	out = std::strtod(t.c_str(), &end);
	return end == t.c_str() + t.size() && errno == 0 && std::isfinite(out);
}

bool IsDigits(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (!std::isdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	return true;
}

} // namespace

XsdNumber ParseXsdNumber(const std::string &text, bool integer) {
	XsdNumber n;
	auto t = AasTrim(text);
	if (StrictNumber(t, n.value)) {
		n.status = integer && std::floor(n.value) != n.value ? XsdNumberStatus::NOT_INTEGER : XsdNumberStatus::OK;
		return n;
	}
	auto comma = t.find(',');
	if (comma == std::string::npos || t.find(',', comma + 1) != std::string::npos || t.find('.') != std::string::npos) {
		return n;
	}
	auto head = t.substr(t[0] == '-' || t[0] == '+' ? 1 : 0, comma - (t[0] == '-' || t[0] == '+' ? 1 : 0));
	auto tail = t.substr(comma + 1);
	if (!IsDigits(head) || !IsDigits(tail)) {
		return n;
	}
	if (tail.size() == 3 && head.size() <= 3) {
		n.status = XsdNumberStatus::AMBIGUOUS; // "1,500": 1.5 or 1500
		return n;
	}
	auto dotted = t;
	dotted[comma] = '.';
	if (StrictNumber(dotted, n.value)) {
		n.status = integer ? XsdNumberStatus::NOT_INTEGER : XsdNumberStatus::DECIMAL_COMMA;
		n.read_as = dotted;
	}
	return n;
}

bool ParseXsdBoolean(const std::string &text, bool &value) {
	auto t = AasTrim(text);
	value = t == "true" || t == "1";
	return value || t == "false" || t == "0";
}

} // namespace anofox
} // namespace duckdb

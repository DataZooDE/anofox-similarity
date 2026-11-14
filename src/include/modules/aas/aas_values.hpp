#pragma once

#include <string>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Typed AAS values (XSD lexical forms, IDTA-01001 DataTypeDefXsd). Pure functions.
//------------------------------------------------------------------------------

std::string AasTrim(const std::string &s);
std::string AasLower(std::string s);

bool IsNumericXsdType(const std::string &value_type);
bool IsIntegerXsdType(const std::string &value_type);

enum class XsdNumberStatus {
	OK,
	DECIMAL_COMMA, // "2,5": not XSD, read as 2.5
	AMBIGUOUS,     // "1,500": thousands or decimal separator, not read
	NOT_INTEGER,   // "1.5" in an integer type
	INVALID        // not a number ("n/a", "")
};

struct XsdNumber {
	XsdNumberStatus status = XsdNumberStatus::INVALID;
	double value = 0;
	std::string read_as; // the text that was parsed, for DECIMAL_COMMA
};

//! An XSD number. A comma is taken as decimal separator only when it cannot be a thousands
//! separator (one comma, no dot, not exactly three digits after it).
XsdNumber ParseXsdNumber(const std::string &text, bool integer);

//! An AAS identifier for a Part 2 REST path or an id segment: base64url without padding.
std::string AasBase64Url(const std::string &in);

//! xs:boolean: true, false, 1, 0 (XSD lexical space). Anything else is false with ok = false.
bool ParseXsdBoolean(const std::string &text, bool &value);

} // namespace anofox
} // namespace duckdb

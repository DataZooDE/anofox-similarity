#include "modules/quantity.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/list_vector.hpp"
#include "duckdb_compat/struct_vector.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "compat_anofox/macro_doc.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

namespace {

const double NO_FACTOR = std::numeric_limits<double>::quiet_NaN();

// SI prefixes used by the seed
struct Prefix {
	const char *symbol;
	double factor;
};

void AddPrefixed(vector<UnitDef> &out, const string &base, const string &dimension,
                 std::initializer_list<Prefix> prefixes, std::initializer_list<const char *> base_aliases = {}) {
	out.push_back({base, vector<string>(base_aliases.begin(), base_aliases.end()), dimension, 1.0, 0.0});
	for (auto &p : prefixes) {
		vector<string> aliases;
		string symbol = string(p.symbol) + base;
		if (string(p.symbol) == "µ") {
			aliases.push_back("u" + base);
		}
		out.push_back({symbol, aliases, dimension, p.factor, 0.0});
	}
}

vector<UnitDef> SeedUnits() {
	vector<UnitDef> u;
	// length
	u.push_back({"m", {}, "length", 1.0, 0});
	u.push_back({"km", {}, "length", 1e3, 0});
	u.push_back({"cm", {}, "length", 1e-2, 0});
	u.push_back({"mm", {}, "length", 1e-3, 0});
	u.push_back({"µm", {"um"}, "length", 1e-6, 0});
	u.push_back({"nm", {}, "length", 1e-9, 0});
	// mass
	u.push_back({"kg", {}, "mass", 1.0, 0});
	u.push_back({"g", {}, "mass", 1e-3, 0});
	u.push_back({"mg", {}, "mass", 1e-6, 0});
	u.push_back({"t", {}, "mass", 1e3, 0});
	// time
	u.push_back({"s", {"sec", "second", "seconds", "Sekunde", "Sekunden"}, "time", 1.0, 0});
	u.push_back({"ms", {}, "time", 1e-3, 0});
	u.push_back({"µs", {"us"}, "time", 1e-6, 0});
	u.push_back({"min", {"minute", "minutes", "Minute", "Minuten"}, "time", 60.0, 0});
	u.push_back({"h", {"hour", "hours", "Stunde", "Stunden"}, "time", 3600.0, 0});
	u.push_back({"d", {"day", "days", "Tag", "Tage", "Tagen"}, "time", 86400.0, 0});
	u.push_back({"week", {"weeks", "Woche", "Wochen", "wk"}, "time", 604800.0, 0});
	u.push_back({"month", {"months", "Monat", "Monate", "Monaten"}, "time", 2629746.0, 0});
	u.push_back({"year", {"years", "Jahr", "Jahre", "Jahren", "a"}, "time", 31556952.0, 0});
	// electrical
	AddPrefixed(u, "A", "current", {{"m", 1e-3}, {"µ", 1e-6}, {"k", 1e3}});
	AddPrefixed(u, "V", "voltage", {{"m", 1e-3}, {"µ", 1e-6}, {"k", 1e3}});
	AddPrefixed(u, "W", "power", {{"m", 1e-3}, {"k", 1e3}, {"M", 1e6}});
	AddPrefixed(u, "Ω", "resistance", {{"m", 1e-3}, {"k", 1e3}, {"M", 1e6}}, {"Ohm", "ohm"});
	AddPrefixed(u, "F", "capacitance", {{"m", 1e-3}, {"µ", 1e-6}, {"n", 1e-9}, {"p", 1e-12}});
	AddPrefixed(u, "Hz", "frequency", {{"k", 1e3}, {"M", 1e6}, {"G", 1e9}});
	// energy
	AddPrefixed(u, "J", "energy", {{"k", 1e3}, {"M", 1e6}});
	u.push_back({"Wh", {}, "energy", 3600.0, 0});
	u.push_back({"kWh", {}, "energy", 3.6e6, 0});
	// mechanics
	AddPrefixed(u, "N", "force", {{"m", 1e-3}, {"k", 1e3}, {"M", 1e6}});
	u.push_back({"N*m", {"N·m", "Nm", "N m"}, "torque", 1.0, 0});
	u.push_back({"kN*m", {"kN·m", "kNm"}, "torque", 1e3, 0});
	AddPrefixed(u, "Pa", "pressure", {{"k", 1e3}, {"M", 1e6}});
	u.push_back({"bar", {}, "pressure", 1e5, 0});
	u.push_back({"mbar", {}, "pressure", 1e2, 0});
	u.push_back({"m/s", {}, "speed", 1.0, 0});
	u.push_back({"mm/s", {}, "speed", 1e-3, 0});
	u.push_back({"cm/s", {}, "speed", 1e-2, 0});
	u.push_back({"km/h", {}, "speed", 1.0 / 3.6, 0});
	u.push_back({"deg", {"°", "Grad"}, "angle", 1.0, 0});
	u.push_back({"rad", {}, "angle", 180.0 / 3.14159265358979323846, 0});
	u.push_back({"m2", {"m²"}, "area", 1.0, 0});
	u.push_back({"cm2", {"cm²"}, "area", 1e-4, 0});
	u.push_back({"mm2", {"mm²"}, "area", 1e-6, 0});
	u.push_back({"m3", {"m³"}, "volume", 1.0, 0});
	u.push_back({"l", {"L", "Liter", "litre", "liter"}, "volume", 1e-3, 0});
	u.push_back({"ml", {"mL"}, "volume", 1e-6, 0});
	u.push_back({"m3/s", {"m³/s"}, "volume_flow", 1.0, 0});
	u.push_back({"m3/h", {"m³/h"}, "volume_flow", 1.0 / 3600.0, 0});
	u.push_back({"l/min", {"L/min"}, "volume_flow", 1e-3 / 60.0, 0});
	u.push_back({"l/h", {"L/h"}, "volume_flow", 1e-3 / 3600.0, 0});
	u.push_back({"kg/m3", {"kg/m³"}, "density", 1.0, 0});
	u.push_back({"g/l", {"g/L"}, "density", 1.0, 0});
	u.push_back({"g/cm3", {"g/cm³"}, "density", 1e3, 0});
	// temperature (affine)
	u.push_back({"K", {}, "temperature", 1.0, 0});
	u.push_back({"°C", {"degC", "℃"}, "temperature", 1.0, 273.15});
	u.push_back({"°F", {"degF", "℉"}, "temperature", 5.0 / 9.0, 273.15 - 32.0 * 5.0 / 9.0});
	// levels and ratios
	u.push_back({"dB(A)", {"dBA", "dB"}, "sound_level", 1.0, 0});
	u.push_back({"%", {"percent", "Prozent"}, "percent", 1.0, 0});
	u.push_back({"ppm", {}, "percent", 1e-4, 0});
	// counts
	u.push_back({"1", {"cycles", "cycle", "Zyklen", "Zyklus", "pcs", "Stück", "x"}, "count", 1.0, 0});
	// data
	u.push_back({"B", {"byte", "bytes"}, "data", 1.0, 0});
	u.push_back({"bit", {"bits"}, "data", 0.125, 0});
	u.push_back({"kB", {}, "data", 1e3, 0});
	u.push_back({"MB", {}, "data", 1e6, 0});
	u.push_back({"GB", {}, "data", 1e9, 0});
	u.push_back({"TB", {}, "data", 1e12, 0});
	u.push_back({"bit/s", {"bps"}, "data_rate", 1.0, 0});
	u.push_back({"kbit/s", {"kbps"}, "data_rate", 1e3, 0});
	u.push_back({"Mbit/s", {"Mbps"}, "data_rate", 1e6, 0});
	u.push_back({"Gbit/s", {"Gbps"}, "data_rate", 1e9, 0});
	// currencies: never converted
	u.push_back({"EUR", {"€", "Euro", "euro"}, "currency", NO_FACTOR, 0});
	u.push_back({"USD", {"$", "US$"}, "currency", NO_FACTOR, 0});
	u.push_back({"GBP", {"£"}, "currency", NO_FACTOR, 0});
	u.push_back({"CHF", {}, "currency", NO_FACTOR, 0});
	u.push_back({"JPY", {"¥"}, "currency", NO_FACTOR, 0});
	u.push_back({"CNY", {"RMB"}, "currency", NO_FACTOR, 0});
	return u;
}

bool IsAsciiAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}
bool AllAlpha(const string &s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return IsAsciiAlpha(c); });
}
string AsciiLower(string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	return s;
}
//! UTF-8 aware replace-all.
void ReplaceAll(string &s, const string &from, const string &to) {
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

bool LinearDimension(const string &dimension) {
	return dimension == "currency" || dimension == "time" || dimension == "percent" || dimension == "sound_level";
}
bool AffineDimension(const string &dimension) {
	return dimension == "temperature";
}

const double AFFINE_TOL = 1.0;  // K
const double AFFINE_BAND = 2.5; // K
const double POINT_WIDEN = 0.01; // decades

} // namespace

//------------------------------------------------------------------------------
// UnitTable
//------------------------------------------------------------------------------

void UnitTable::Add(UnitDef unit) {
	for (auto &u : units) {
		if (u.symbol == unit.symbol) {
			u = std::move(unit); // a user unit overrides the seed
			return;
		}
	}
	units.push_back(std::move(unit));
}

void UnitTable::Index() {
	by_alias.clear();
	by_lower_alias.clear();
	base_units.clear();
	max_alias_length = 0;
	for (idx_t i = 0; i < units.size(); i++) {
		auto &u = units[i];
		vector<string> names {u.symbol};
		names.insert(names.end(), u.aliases.begin(), u.aliases.end());
		for (auto &n : names) {
			by_alias[n] = i;
			if (n.size() >= 3 && AllAlpha(n)) {
				by_lower_alias.emplace(AsciiLower(n), i);
			}
			max_alias_length = MaxValue<idx_t>(max_alias_length, n.size());
		}
		if (!std::isnan(u.factor) && u.factor == 1.0 && u.offset == 0.0 && !base_units.count(u.dimension)) {
			base_units[u.dimension] = u.symbol;
		}
	}
}

const UnitTable &UnitTable::Seed() {
	static const UnitTable seed = [] {
		UnitTable t;
		for (auto &u : SeedUnits()) {
			t.Add(u);
		}
		t.Index();
		return t;
	}();
	return seed;
}

UnitTable UnitTable::WithUserUnits(const Value &list) {
	UnitTable t = Seed();
	if (list.IsNull()) {
		return t;
	}
	if (list.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException("unit_table must be a LIST of STRUCT(symbol, alias, dimension, factor, offset), "
		                            "e.g. (SELECT list(u) FROM my_units u)");
	}
	for (auto &entry : ListValue::GetChildren(list)) {
		if (entry.IsNull() || entry.type().id() != LogicalTypeId::STRUCT) {
			continue;
		}
		auto &child_types = StructType::GetChildTypes(entry.type());
		auto &values = StructValue::GetChildren(entry);
		UnitDef u {"", {}, "", 1.0, 0.0};
		for (idx_t i = 0; i < child_types.size(); i++) {
			auto name = StringUtil::Lower(static_cast<const string &>(child_types[i].first));
			auto &v = values[i];
			if (name == "symbol" && !v.IsNull()) {
				u.symbol = v.ToString();
			} else if (name == "alias" && !v.IsNull()) {
				if (v.type().id() == LogicalTypeId::LIST) {
					for (auto &a : ListValue::GetChildren(v)) {
						if (!a.IsNull()) {
							u.aliases.push_back(a.ToString());
						}
					}
				} else {
					u.aliases.push_back(v.ToString());
				}
			} else if (name == "dimension" && !v.IsNull()) {
				u.dimension = v.ToString();
			} else if (name == "factor") {
				u.factor = v.IsNull() ? NO_FACTOR : v.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			} else if (name == "offset" && !v.IsNull()) {
				u.offset = v.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			}
		}
		if (u.symbol.empty() || u.dimension.empty()) {
			throw InvalidInputException("unit_table entries need a symbol and a dimension");
		}
		t.Add(std::move(u));
	}
	t.Index();
	return t;
}

const UnitDef *UnitTable::Find(const string &token) const {
	auto it = by_alias.find(token);
	if (it != by_alias.end()) {
		return &units[it->second];
	}
	if (token.size() >= 3 && AllAlpha(token)) {
		auto lit = by_lower_alias.find(AsciiLower(token));
		if (lit != by_lower_alias.end()) {
			return &units[lit->second];
		}
	}
	return nullptr;
}

const UnitDef *UnitTable::LongestPrefix(const string &text, idx_t &matched) const {
	for (idx_t len = MinValue<idx_t>(text.size(), max_alias_length); len > 0; len--) {
		auto prefix = text.substr(0, len);
		auto unit = Find(prefix);
		if (!unit) {
			continue;
		}
		// the unit must end at a word boundary: not followed by a letter or digit
		if (len < text.size()) {
			char next = text[len];
			if (IsAsciiAlpha(next) || IsDigit(next) || (unsigned char)next >= 0x80) {
				continue;
			}
		}
		matched = len;
		return unit;
	}
	return nullptr;
}

string UnitTable::BaseUnit(const string &dimension) const {
	auto it = base_units.find(dimension);
	return it == base_units.end() ? string() : it->second;
}

//------------------------------------------------------------------------------
// Parsing
//------------------------------------------------------------------------------

namespace {

struct NumberToken {
	idx_t begin;
	idx_t end;
	double value;
};

//! Interpret a digit run with '.' and ',' separators for the primary language.
double ParseNumberText(const string &raw, const string &primary_lang) {
	bool negative = !raw.empty() && raw[0] == '-';
	string s = negative ? raw.substr(1) : raw;
	auto commas = std::count(s.begin(), s.end(), ',');
	auto dots = std::count(s.begin(), s.end(), '.');
	string digits;
	if (commas && dots) {
		char decimal = s.find_last_of(".,") == s.rfind(',') ? ',' : '.';
		for (auto c : s) {
			if (IsDigit(c)) {
				digits += c;
			} else if (c == decimal) {
				digits += '.';
			}
		}
	} else if (commas + dots == 0) {
		digits = s;
	} else {
		char sep = commas ? ',' : '.';
		auto count = commas ? commas : dots;
		auto pos = s.find(sep);
		auto after = s.size() - pos - 1;
		bool thousands;
		if (count > 1) {
			thousands = true;
		} else if (after != 3 || pos > 3) {
			thousands = false;
		} else {
			// exactly three digits after one separator: the language decides
			thousands = primary_lang == "de" ? sep == '.' : sep == ',';
		}
		for (auto c : s) {
			if (IsDigit(c)) {
				digits += c;
			} else if (c == sep && !thousands) {
				digits += '.';
			}
		}
	}
	double v = std::strtod(digits.c_str(), nullptr);
	return negative ? -v : v;
}

//! Numbers in text; a digit right after a letter (IP54, F1) does not start a number.
vector<NumberToken> FindNumbers(const string &s, const string &primary_lang) {
	vector<NumberToken> out;
	idx_t i = 0;
	while (i < s.size()) {
		if (!IsDigit(s[i])) {
			i++;
			continue;
		}
		if (i > 0 && (IsAsciiAlpha(s[i - 1]) || IsDigit(s[i - 1]))) {
			// part of a code such as IP54
			while (i < s.size() && (IsDigit(s[i]) || IsAsciiAlpha(s[i]))) {
				i++;
			}
			continue;
		}
		idx_t begin = i;
		bool negative = begin > 0 && s[begin - 1] == '-' && (begin == 1 || s[begin - 2] == ' ' || s[begin - 2] == '(');
		idx_t j = i;
		while (j < s.size()) {
			if (IsDigit(s[j])) {
				j++;
			} else if ((s[j] == '.' || s[j] == ',') && j + 1 < s.size() && IsDigit(s[j + 1])) {
				j++;
			} else {
				break;
			}
		}
		auto raw = s.substr(begin, j - begin);
		if (negative) {
			raw = "-" + raw;
			begin--;
		}
		out.push_back({begin, j, ParseNumberText(raw, primary_lang)});
		i = j;
	}
	return out;
}

bool ContainsAny(const string &haystack, std::initializer_list<const char *> needles) {
	for (auto n : needles) {
		if (haystack.find(n) != string::npos) {
			return true;
		}
	}
	return false;
}

string Trim(const string &s) {
	idx_t b = 0, e = s.size();
	while (b < e && s[b] == ' ') {
		b++;
	}
	while (e > b && s[e - 1] == ' ') {
		e--;
	}
	return s.substr(b, e - b);
}

//! The unit right after position pos (skipping spaces); token is what was written.
const UnitDef *UnitAfter(const string &s, idx_t pos, const UnitTable &units, string &token, idx_t &end) {
	while (pos < s.size() && s[pos] == ' ') {
		pos++;
	}
	end = pos;
	token.clear();
	if (pos >= s.size()) {
		return nullptr;
	}
	idx_t matched = 0;
	auto unit = units.LongestPrefix(s.substr(pos), matched);
	if (unit) {
		token = s.substr(pos, matched);
		end = pos + matched;
		return unit;
	}
	// an unknown word right after the number: keep it, never guess
	idx_t j = pos;
	while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != ';') {
		j++;
	}
	auto word = s.substr(pos, j - pos);
	while (!word.empty() && (word.back() == '.' || word.back() == ')' || word.back() == ':')) {
		word.pop_back();
	}
	auto lower_word = AsciiLower(word);
	bool connector = lower_word == "to" || lower_word == "and" || lower_word == "bis" || lower_word == "und" ||
	                 lower_word == "or" || lower_word == "oder";
	if (!word.empty() && !connector && (IsAsciiAlpha(word[0]) || (unsigned char)word[0] >= 0x80) && word != "~") {
		token = word;
		end = pos + word.size();
	}
	return nullptr;
}

} // namespace

Quantity ParseQuantity(const string &text, const vector<string> &langs, const UnitTable &units) {
	Quantity q;
	string primary = langs.empty() ? "en" : StringUtil::Lower(langs[0]);
	string s = text;
	ReplaceAll(s, " ", " ");
	ReplaceAll(s, " ", " ");
	ReplaceAll(s, "−", "-");
	ReplaceAll(s, "–", "~");
	ReplaceAll(s, "—", "~");
	ReplaceAll(s, "…", "~");
	ReplaceAll(s, "...", "~");
	auto numbers = FindNumbers(s, primary);
	auto lower = AsciiLower(s);

	if (numbers.empty()) {
		// an ordinal class: upper-case letters followed by digits (IP54, F1)
		idx_t i = 0;
		while (i < s.size()) {
			if ((i == 0 || s[i - 1] == ' ') && s[i] >= 'A' && s[i] <= 'Z') {
				idx_t j = i;
				while (j < s.size() && s[j] >= 'A' && s[j] <= 'Z') {
					j++;
				}
				idx_t k = j;
				while (k < s.size() && IsDigit(s[k])) {
					k++;
				}
				if (k > j && j - i <= 4 && (k == s.size() || s[k] == ' ' || s[k] == ',' || s[k] == '.')) {
					q.category = s.substr(i, k - i);
					break;
				}
				i = j;
			} else {
				i++;
			}
		}
		if (!q.category.empty()) {
			if (ContainsAny(lower, {"or better", "oder besser", "at least", "min.", "mindestens", "minimum", "≥", ">="})) {
				q.bound = "at_least";
			} else if (ContainsAny(lower, {"or worse", "at most", "max.", "höchstens", "≤", "<="})) {
				q.bound = "at_most";
			}
		}
		return q;
	}

	auto &first = numbers[0];
	string prefix = AsciiLower(s.substr(0, first.begin));
	string token1, token2;
	idx_t end1 = 0, end2 = 0;
	auto unit1 = UnitAfter(s, first.end, units, token1, end1);

	// a second number that forms a range with the first
	bool range = false;
	bool tolerance = false;
	const UnitDef *unit2 = nullptr;
	double v2 = 0;
	if (numbers.size() >= 2) {
		auto &second = numbers[1];
		auto between = AsciiLower(Trim(s.substr(end1, second.begin - end1)));
		if (between == "~" || between == "-" || between == "to" || between == "bis" ||
		    ((between == "and" || between == "und") && ContainsAny(prefix, {"between", "zwischen"}))) {
			range = true;
			v2 = second.value;
			unit2 = UnitAfter(s, second.end, units, token2, end2);
		} else if (between.find("±") != string::npos || between.find("+/-") != string::npos) {
			tolerance = true;
		}
	}

	const UnitDef *unit = unit1;
	string token = token1;
	if (range && unit2) {
		unit = unit2;
		token = token2;
	}
	// a currency symbol written before the number
	if (!unit && token.empty()) {
		auto trimmed = Trim(s.substr(0, first.begin));
		for (auto sym : {"€", "$", "£", "¥"}) {
			string ss(sym);
			if (trimmed.size() >= ss.size() && trimmed.compare(trimmed.size() - ss.size(), ss.size(), ss) == 0) {
				unit = units.Find(ss);
				token = ss;
				break;
			}
		}
	}

	double lo = first.value, hi = range ? v2 : first.value;
	if (lo > hi) {
		std::swap(lo, hi);
	}
	q.has_value = true;
	q.source_unit = token;
	if (unit) {
		q.dimension = unit->dimension;
		if (std::isnan(unit->factor)) {
			q.lo = lo;
			q.hi = hi;
			q.unit = unit->symbol;
		} else {
			q.lo = lo * unit->factor + unit->offset;
			q.hi = hi * unit->factor + unit->offset;
			q.unit = units.BaseUnit(unit->dimension);
		}
	} else if (token.empty()) {
		q.lo = lo;
		q.hi = hi;
		q.dimension = "count";
		q.unit = "1";
	} else {
		q.lo = lo;
		q.hi = hi;
		q.unit = token; // unknown: never guessed
	}

	string suffix = AsciiLower(s.substr(range ? MaxValue(end2, numbers[1].end) : end1));
	if (range) {
		q.bound = "within";
	} else if (tolerance || ContainsAny(prefix, {"nominal", "nennwert", "="})) {
		q.bound = "equals";
	} else if (ContainsAny(prefix, {"at most", "max", "höchstens", "bis zu", "up to", "not more than",
	                                "nicht mehr als", "≤", "<="})) {
		q.bound = "at_most";
	} else if (ContainsAny(prefix, {"at least", "min", "mindestens", "not less than", "nicht weniger als", "≥",
	                                ">="}) ||
	           ContainsAny(suffix, {"or better", "oder besser", "or more", "oder mehr"})) {
		q.bound = "at_least";
	} else if (ContainsAny(suffix, {"or less", "oder weniger"})) {
		q.bound = "at_most";
	}
	return q;
}

//------------------------------------------------------------------------------
// Value comparison
//------------------------------------------------------------------------------

ValueScore ValueSatisfies(const ValueSide &req, const ValueSide &prov, const ValueOptions &options) {
	ValueScore r;
	if (req.dimension.empty() || prov.dimension.empty() || req.dimension != prov.dimension ||
	    (req.dimension == "currency" && !req.unit.empty() && !prov.unit.empty() && req.unit != prov.unit)) {
		r.rule = "gate";
		return r;
	}
	if (!req.has_lo && !req.has_hi) {
		r.rule = "missing";
		return r;
	}
	if (!prov.has_lo && !prov.has_hi) {
		r.rule = "missing";
		return r;
	}
	double r_lo = req.has_lo ? req.lo : req.hi, r_hi = req.has_hi ? req.hi : req.lo;
	double p_lo = prov.has_lo ? prov.lo : prov.hi, p_hi = prov.has_hi ? prov.hi : prov.lo;
	string bound = req.bound.empty() ? options.default_bound : req.bound;
	r.has_distance = true;

	if (AffineDimension(req.dimension)) {
		auto soft = [](double d) {
			return d <= AFFINE_TOL ? 1.0 : MaxValue(0.0, 1.0 - (d - AFFINE_TOL) / AFFINE_BAND);
		};
		if (bound == "at_least") {
			r.distance = MaxValue(0.0, r_lo - p_hi);
			r.score = soft(r.distance);
		} else if (bound == "at_most") {
			r.distance = MaxValue(0.0, p_lo - r_hi);
			r.score = soft(r.distance);
		} else if (bound == "equals") {
			r.distance = MinValue(std::fabs(r_lo - p_lo), std::fabs(r_lo - p_hi));
			r.score = soft(r.distance);
		} else {
			double a = bound.empty() ? r_lo - AFFINE_TOL : r_lo, b = bound.empty() ? r_hi + AFFINE_TOL : r_hi;
			double width = b - a;
			double inter = MaxValue(0.0, MinValue(b, p_hi + AFFINE_TOL) - MaxValue(a, p_lo - AFFINE_TOL));
			r.score = width <= 0 ? 1.0 : inter / width;
			r.distance = 1.0 - r.score;
		}
		r.rule = bound.empty() ? "point" : bound;
		return r;
	}

	bool log_scale = options.scale.empty() ? !LinearDimension(req.dimension) : options.scale == "log";
	log_scale = log_scale && r_lo > 0 && p_lo > 0;
	double tol_rel = log_scale ? 1.0 - std::pow(10.0, -options.tol) : options.tol;
	auto soften_with = [&](double d, double tol) {
		if (d <= tol) {
			return 1.0;
		}
		return options.soft_band <= 0 ? 0.0 : MaxValue(0.0, 1.0 - (d - tol) / options.soft_band);
	};
	if (bound == "at_least" || bound == "at_most" || bound == "equals") {
		double v = bound == "at_most" ? r_hi : r_lo;
		double p = bound == "at_least" ? p_hi : p_lo;
		if (bound == "equals") {
			p = std::fabs(p_lo - r_lo) >= std::fabs(p_hi - r_lo) ? p_lo : p_hi; // the farther end
		}
		double miss = bound == "at_least" ? v - p : (bound == "at_most" ? p - v : std::fabs(v - p));
		miss = MaxValue(0.0, miss);
		if (v == 0) {
			// a zero limit: absolute distance, strict score
			r.distance = miss;
			r.score = miss == 0 ? 1.0 : 0.0;
		} else {
			r.distance = miss / std::fabs(v);
			// equals is relative on every scale (HLD §4.2 '(relative)', §11.7)
			r.score = soften_with(r.distance, bound == "equals" ? options.tol : tol_rel);
		}
	} else {
		// within, or an unqualified point widened to a small interval
		bool point = bound.empty();
		double a, b, c, e;
		if (log_scale) {
			if (point) {
				r_lo *= std::pow(10.0, -POINT_WIDEN);
				r_hi *= std::pow(10.0, POINT_WIDEN);
			}
			a = std::log10(r_lo);
			b = std::log10(r_hi);
			c = std::log10(p_lo) - options.tol;
			e = std::log10(p_hi) + options.tol;
		} else {
			if (point) {
				r_lo *= 1.0 - options.tol / 2;
				r_hi *= 1.0 + options.tol / 2;
			}
			a = r_lo;
			b = r_hi;
			c = p_lo - options.tol * std::fabs(p_lo);
			e = p_hi + options.tol * std::fabs(p_hi);
		}
		double width = b - a;
		double inter = MaxValue(0.0, MinValue(b, e) - MaxValue(a, c));
		r.score = width > 0 ? inter / width : ((c <= a && a <= e) ? 1.0 : 0.0);
		r.distance = 1.0 - r.score;
	}
	r.rule = bound.empty() ? "point" : bound;
	return r;
}

//------------------------------------------------------------------------------
// SQL functions
//------------------------------------------------------------------------------

namespace {

LogicalType QuantityType() {
	child_list_t<LogicalType> children {{"lo", LogicalType::DOUBLE},          {"hi", LogicalType::DOUBLE},
	                                    {"unit", LogicalType::VARCHAR},       {"dimension", LogicalType::VARCHAR},
	                                    {"bound", LogicalType::VARCHAR},      {"raw", LogicalType::VARCHAR},
	                                    {"source_unit", LogicalType::VARCHAR}, {"category", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(children);
}

LogicalType NormalizedType() {
	child_list_t<LogicalType> children {
	    {"value", LogicalType::DOUBLE}, {"unit", LogicalType::VARCHAR}, {"dimension", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(children);
}

LogicalType ValueScoreType() {
	child_list_t<LogicalType> children {
	    {"score", LogicalType::DOUBLE}, {"distance", LogicalType::DOUBLE}, {"rule", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(children);
}

Value OptString(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

vector<string> LangsOf(const Value &v) {
	vector<string> out;
	if (v.IsNull()) {
		return {"en", "de"};
	}
	if (v.type().id() == LogicalTypeId::LIST) {
		for (auto &c : ListValue::GetChildren(v)) {
			if (!c.IsNull()) {
				out.push_back(StringUtil::Lower(c.ToString()));
			}
		}
	} else {
		for (auto &part : StringUtil::Split(v.ToString(), ',')) {
			out.push_back(StringUtil::Lower(Trim(part)));
		}
	}
	for (auto &l : out) {
		if (l != "en" && l != "de") {
			throw InvalidInputException("lang must be 'en' and/or 'de', got '%s'", l);
		}
	}
	if (out.empty()) {
		out = {"en", "de"};
	}
	return out;
}

//! Unit table for row i: the seed, or seed + user units (cached when constant).
struct UnitCache {
	bool constant_ready = false;
	UnitTable table;
	const UnitTable &Get(DataChunk &args, idx_t col, idx_t row) {
		if (args.ColumnCount() <= col) {
			return UnitTable::Seed();
		}
		auto &vec = args.data[col];
		if (vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (!constant_ready) {
				table = UnitTable::WithUserUnits(vec.GetValue(0));
				constant_ready = true;
			}
			return table;
		}
		table = UnitTable::WithUserUnits(vec.GetValue(row));
		return table;
	}
};

void ParseQuantityFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	UnitCache units;
	vector<string> langs {"en", "de"};
	bool lang_constant = args.ColumnCount() < 2 || args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (args.ColumnCount() >= 2 && lang_constant) {
		langs = LangsOf(args.data[1].GetValue(0));
	}
	for (idx_t i = 0; i < count; i++) {
		auto text = args.data[0].GetValue(i);
		if (text.IsNull()) {
			result.SetValue(i, Value(QuantityType()));
			continue;
		}
		if (!lang_constant) {
			langs = LangsOf(args.data[1].GetValue(i));
		}
		auto raw = text.ToString();
		auto q = ParseQuantity(raw, langs, units.Get(args, 2, i));
		child_list_t<Value> v;
		v.emplace_back("lo", q.has_value ? Value::DOUBLE(q.lo) : Value(LogicalType::DOUBLE));
		v.emplace_back("hi", q.has_value ? Value::DOUBLE(q.hi) : Value(LogicalType::DOUBLE));
		v.emplace_back("unit", OptString(q.unit));
		v.emplace_back("dimension", OptString(q.dimension));
		v.emplace_back("bound", OptString(q.bound));
		v.emplace_back("raw", Value(raw));
		v.emplace_back("source_unit", OptString(q.source_unit));
		v.emplace_back("category", OptString(q.category));
		result.SetValue(i, Value::STRUCT(std::move(v)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void NormalizeQuantityFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	UnitCache units;
	for (idx_t i = 0; i < count; i++) {
		auto value = args.data[0].GetValue(i);
		auto unit = args.data[1].GetValue(i);
		if (value.IsNull()) {
			result.SetValue(i, Value(NormalizedType()));
			continue;
		}
		double v = value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		child_list_t<Value> out;
		const UnitDef *def = unit.IsNull() ? nullptr : units.Get(args, 2, i).Find(Trim(unit.ToString()));
		if (!def) {
			out.emplace_back("value", Value::DOUBLE(v));
			out.emplace_back("unit", unit.IsNull() ? Value(LogicalType::VARCHAR) : Value(unit.ToString()));
			out.emplace_back("dimension", Value(LogicalType::VARCHAR));
		} else if (std::isnan(def->factor)) {
			out.emplace_back("value", Value::DOUBLE(v));
			out.emplace_back("unit", Value(def->symbol));
			out.emplace_back("dimension", Value(def->dimension));
		} else {
			out.emplace_back("value", Value::DOUBLE(v * def->factor + def->offset));
			out.emplace_back("unit", Value(units.Get(args, 2, i).BaseUnit(def->dimension)));
			out.emplace_back("dimension", Value(def->dimension));
		}
		result.SetValue(i, Value::STRUCT(std::move(out)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ValueSide SideOf(const Value &v, bool requirement) {
	ValueSide side;
	if (v.type().id() != LogicalTypeId::STRUCT) {
		throw InvalidInputException("value_satisfies expects STRUCT arguments with lo, hi and dimension, got %s",
		                            v.type().ToString());
	}
	auto &child_types = StructType::GetChildTypes(v.type());
	auto &values = StructValue::GetChildren(v);
	bool has_lo_field = false;
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto name = StringUtil::Lower(static_cast<const string &>(child_types[i].first));
		auto &c = values[i];
		if (name == "lo") {
			has_lo_field = true;
			if (!c.IsNull()) {
				side.has_lo = true;
				side.lo = c.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			}
		} else if (name == "hi" && !c.IsNull()) {
			side.has_hi = true;
			side.hi = c.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		} else if (name == "dimension" && !c.IsNull()) {
			side.dimension = c.ToString();
		} else if (name == "unit" && !c.IsNull()) {
			side.unit = c.ToString();
		} else if (name == "bound" && !c.IsNull() && requirement) {
			side.bound = c.ToString();
		} else if (name == "category" && !c.IsNull()) {
			throw InvalidInputException("value_satisfies compares numbers; for categorical values such as '%s' "
			                            "use compat_score",
			                            c.ToString());
		}
	}
	if (!has_lo_field) {
		throw InvalidInputException("value_satisfies: a %s needs a field 'lo'", requirement ? "requirement" : "provided value");
	}
	// a value given in a non-base unit of its dimension is normalised first (250 mm -> 0.25 m)
	if (!side.unit.empty() && side.dimension != "currency") {
		auto &seed = UnitTable::Seed();
		auto def = seed.Find(side.unit);
		if (def && def->dimension == side.dimension && !std::isnan(def->factor)) {
			side.lo = side.lo * def->factor + def->offset;
			side.hi = side.hi * def->factor + def->offset;
			side.unit = seed.BaseUnit(side.dimension);
		}
	}
	return side;
}

void CheckBound(const string &bound) {
	if (!bound.empty() && bound != "at_least" && bound != "at_most" && bound != "within" && bound != "equals") {
		throw InvalidInputException("value_satisfies: unknown bound '%s' (expected at_least, at_most, within, equals)",
		                            bound);
	}
}

void ValueSatisfiesFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto req = args.data[0].GetValue(i);
		auto prov = args.data[1].GetValue(i);
		if (req.IsNull() || prov.IsNull()) {
			result.SetValue(i, Value(ValueScoreType()));
			continue;
		}
		ValueOptions options;
		if (args.ColumnCount() >= 3) {
			auto scale = args.data[2].GetValue(i);
			if (!scale.IsNull()) {
				options.scale = StringUtil::Lower(scale.ToString());
				if (options.scale != "log" && options.scale != "linear") {
					throw InvalidInputException("value_satisfies: scale must be 'log' or 'linear', got '%s'",
					                            options.scale);
				}
			}
		}
		if (args.ColumnCount() >= 5) {
			auto tol = args.data[3].GetValue(i);
			auto band = args.data[4].GetValue(i);
			if (!tol.IsNull()) {
				options.tol = tol.GetValue<double>();
			}
			if (!band.IsNull()) {
				options.soft_band = band.GetValue<double>();
			}
			if (!std::isfinite(options.tol) || !std::isfinite(options.soft_band) || options.tol < 0 ||
			    options.soft_band < 0) {
				throw InvalidInputException("value_satisfies: tol and soft_band must be non-negative and finite");
			}
		}
		if (args.ColumnCount() >= 6) {
			auto d = args.data[5].GetValue(i);
			if (!d.IsNull()) {
				options.default_bound = d.ToString();
			}
		}
		auto r_side = SideOf(req, true);
		auto p_side = SideOf(prov, false);
		CheckBound(r_side.bound);
		CheckBound(options.default_bound);
		auto r = ValueSatisfies(r_side, p_side, options);
		child_list_t<Value> v;
		v.emplace_back("score", Value::DOUBLE(r.score));
		v.emplace_back("distance", r.has_distance ? Value::DOUBLE(r.distance) : Value(LogicalType::DOUBLE));
		v.emplace_back("rule", Value(r.rule));
		result.SetValue(i, Value::STRUCT(std::move(v)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> ParseQuantityBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("parse_quantity");
	return nullptr;
}
unique_ptr<FunctionData> NormalizeQuantityBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("normalize_quantity");
	return nullptr;
}
unique_ptr<FunctionData> ValueSatisfiesBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("value_satisfies");
	return nullptr;
}

struct Overload {
	vector<LogicalType> arguments;
	vector<string> names;
	vector<string> examples;
};

void RegisterScalar(ExtensionLoader &loader, const string &name, const LogicalType &ret, scalar_function_t fn,
                    bind_scalar_function_t bind, const string &description, const vector<Overload> &overloads) {
	ScalarFunctionSet set(duckdb_compat::FunctionName(name));
	for (auto &o : overloads) {
		ScalarFunction f(duckdb_compat::FunctionName(name), o.arguments, ret, fn, bind);
		duckdb_compat::SetFallible(f);
		f.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		set.AddFunction(f);
	}
	CreateScalarFunctionInfo info(set);
	for (auto &o : overloads) {
		FunctionDescription desc;
		desc.description = description;
		desc.examples = o.examples;
		desc.categories = {"coverage", "quantity"};
		desc.parameter_names = o.names;
		desc.parameter_types = o.arguments;
		info.descriptions.push_back(std::move(desc));
	}
	duckdb_compat::NameArgumentsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

string SqlDouble(double v) {
	return std::isnan(v) ? "NULL::DOUBLE" : StringUtil::Format("%.17g::DOUBLE", v);
}

unique_ptr<TableRef> QuantityUnitsBindReplace(ClientContext &context, TableFunctionBindInput &) {
	PostHogTelemetry::Instance().RecordFunctionCall("quantity_units");
	auto &seed = UnitTable::Seed();
	string rows;
	for (auto &u : seed.Units()) {
		string aliases;
		for (auto &a : u.aliases) {
			aliases += (aliases.empty() ? "" : ", ") + QuoteSQLStringLiteral(a);
		}
		auto base = std::isnan(u.factor) ? u.symbol : seed.BaseUnit(u.dimension);
		rows += (rows.empty() ? "" : ",\n") +
		        StringUtil::Format("(%s, [%s]::VARCHAR[], %s, %s, %s, %s)", QuoteSQLStringLiteral(u.symbol), aliases,
		                           QuoteSQLStringLiteral(u.dimension), SqlDouble(u.factor), SqlDouble(u.offset),
		                           QuoteSQLStringLiteral(base));
	}
	auto sql = "SELECT * FROM (VALUES " + rows + ") t(symbol, alias, dimension, factor, \"offset\", base_unit)";
	auto statements = duckdb_compat::ParseStatements(context, sql);
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

const char *COMPAT_SCORE_MACRO =
    "(dimension, required, provided, rules_table := 'compatibility_rules') AS "
    "CASE WHEN required IS NULL OR provided IS NULL THEN NULL::DOUBLE ELSE COALESCE("
    "(SELECT max(CAST(r.score AS DOUBLE)) FROM query_table(rules_table) r "
    " WHERE r.dimension = dimension AND r.required = required AND r.provided = provided), "
    "CASE WHEN required = provided THEN 1.0::DOUBLE ELSE 0.0::DOUBLE END) END";

} // namespace

void RegisterQuantityFunctions(ExtensionLoader &loader) {
	auto any = LogicalType::ANY;
	auto vc = LogicalType::VARCHAR;
	auto vcl = LogicalType::LIST(LogicalType::VARCHAR);
	auto dbl = LogicalType::DOUBLE;

	RegisterScalar(
	    loader, "parse_quantity", QuantityType(), DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, ParseQuantityFun),
	    compat_anofox::ScalarBind<&ParseQuantityBind>(),
	    "Parses a quantity written in English or German ('max. 38 dB(A)', 'mindestens 24 cm', '6–18 V', "
	    "'zwischen 15 und 25 mm/s', '220 N ± 2 %', 'IP54 or better') into STRUCT(lo, hi, unit, dimension, bound, raw, "
	    "source_unit, category). lo/hi are in the base unit of the dimension (unit); bound is at_least, at_most, within, "
	    "equals or NULL when the text states none. lang ('en', 'de' or a list, first entry decides ambiguous separators "
	    "such as '2.500') defaults to ['en', 'de']. unit_table is a LIST of STRUCT(symbol, alias, dimension, factor, "
	    "offset) that extends or overrides the seed (see quantity_units()). A bare number is a count; an unknown unit "
	    "keeps the number and has dimension NULL. Pass lang before unit_table (DuckDB 1.5.6 binds by position).",
	    {{{vc}, {"text"}, {"SELECT parse_quantity('max. 38 dB(A)');"}},
	     {{vc, vc}, {"text", "lang"}, {"SELECT parse_quantity('65.000 Zyklen', 'de');"}},
	     {{vc, vcl}, {"text", "lang"}, {"SELECT parse_quantity('2.500 mA', ['de', 'en']);"}},
	     {{vc, vc, any}, {"text", "lang", "unit_table"},
	      {"SELECT parse_quantity('3 Stk.', 'de', (SELECT list(u) FROM my_units u));"}},
	     {{vc, vcl, any}, {"text", "lang", "unit_table"},
	      {"SELECT parse_quantity('3 Stk.', ['de'], (SELECT list(u) FROM my_units u));"}}});

	RegisterScalar(loader, "normalize_quantity", NormalizedType(),
	               DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, NormalizeQuantityFun),
	               compat_anofox::ScalarBind<&NormalizeQuantityBind>(),
	               "Converts a value in a unit to the base unit of its dimension: STRUCT(value, unit, dimension). An "
	               "unknown unit is never guessed: value and unit come back unchanged with dimension NULL. Currencies "
	               "keep their value (same-currency comparison only). unit_table extends the seed as in "
	               "parse_quantity.",
	               {{{dbl, vc}, {"value", "unit"}, {"SELECT normalize_quantity(2500, 'mA'); -- 2.5 A"}},
	                {{dbl, vc, any}, {"value", "unit", "unit_table"},
	                 {"SELECT normalize_quantity(3, 'lanes', (SELECT list(u) FROM my_units u));"}}});

	const string vs_desc =
	    "Direction-aware comparison of a requirement with a provided value (both STRUCTs with lo, hi, dimension; the "
	    "requirement also bound, either may carry unit). Returns STRUCT(score, distance, rule). at_least uses the high "
	    "end of a provided range, at_most the low end, within the share of the required range covered, equals the "
	    "relative difference; an unqualified value is a point widened by ±0.01 decades unless default_bound gives it "
	    "a direction. score is 1 within tol (decades on log scale, relative on linear scale), falling to 0 across "
	    "soft_band; distance is relative (absolute in K for temperature, which uses a 1 K tolerance and 2.5 K band). "
	    "Different dimensions, or different currencies, give rule 'gate' and score 0. scale defaults per dimension "
	    "(linear for time, currency, percent, sound level). Pass the options in order: scale, tol, soft_band, "
	    "default_bound.";
	RegisterScalar(
	    loader, "value_satisfies", ValueScoreType(), DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, ValueSatisfiesFun),
	    compat_anofox::ScalarBind<&ValueSatisfiesBind>(), vs_desc,
	    {{{any, any}, {"requirement", "provided"},
	      {"SELECT value_satisfies(parse_quantity('max. 38 dB(A)'), parse_quantity('35 dB(A)'));"}},
	     {{any, any, vc}, {"requirement", "provided", "scale"},
	      {"SELECT value_satisfies(parse_quantity('at least 24 cm'), parse_quantity('0.235 m'), 'linear');"}},
	     {{any, any, vc, dbl, dbl}, {"requirement", "provided", "scale", "tol", "soft_band"},
	      {"SELECT value_satisfies(parse_quantity('at least 24 cm'), parse_quantity('23 cm'), NULL, 0.05, 0.1);"}},
	     {{any, any, vc, dbl, dbl, vc}, {"requirement", "provided", "scale", "tol", "soft_band", "default_bound"},
	      {"SELECT value_satisfies(parse_quantity('24 cm'), parse_quantity('26 cm'), NULL, 0.02, 0.05, 'at_least');"}}});

	{
		TableFunction f("quantity_units", {}, nullptr, nullptr);
		f.bind_replace = QuantityUnitsBindReplace;
		FunctionDescription desc;
		desc.description = "The built-in, read-only units seed of the quantity layer: symbol, alias (list), "
		                   "dimension, factor, offset (base = value * factor + offset) and base_unit. Currencies "
		                   "have factor NULL and are never converted. Copy it into a table to extend it and pass "
		                   "(SELECT list(u) FROM my_units u) as unit_table.";
		desc.examples = {"SELECT * FROM quantity_units() WHERE dimension = 'length';",
		                 "CREATE TABLE my_units AS SELECT symbol, alias, dimension, factor, \"offset\" FROM "
		                 "quantity_units();"};
		desc.categories = {"coverage", "quantity"};
		CreateTableFunctionInfo info(f);
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}

	compat_anofox::RegisterMacroInfo(
	    loader, duckdb_compat::ScalarMacroInfo(DEFAULT_SCHEMA, "compat_score", COMPAT_SCORE_MACRO),
	    {ddoc::Doc()
	         .Describe("Compatibility of a provided category with a required one for an ordinal or categorical "
	                   "dimension (protection class, certification level, qualifier), looked up in a curated rules "
	                   "table (dimension, required, provided, score). Without a rule: 1 if equal, else 0. Text "
	                   "similarity is never used (BR-15).")
	         .Example("SELECT compat_score('protection_class', 'IP54', 'IP65')")
	         .Example("SELECT compat_score('cert_level', 'silver', 'gold', rules_table := 'my_rules')")},
	    {"coverage", "quantity"});
}

} // namespace anofox
} // namespace duckdb

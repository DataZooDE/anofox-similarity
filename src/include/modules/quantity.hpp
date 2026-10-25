#pragma once

#include "duckdb.hpp"

#include <unordered_map>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Quantity layer (coverage matching, HLD §4.2 with the §11 amendments)
//------------------------------------------------------------------------------

//! One unit: base = value * factor + offset. A NaN factor (currency) is never converted.
struct UnitDef {
	string symbol;
	vector<string> aliases;
	string dimension;
	double factor;
	double offset;
};

//! A unit lookup: the built-in seed, optionally extended or overridden by user units.
class UnitTable {
public:
	static const UnitTable &Seed();
	//! The seed plus user units (a LIST of STRUCT(symbol, alias, dimension, factor, offset)).
	static UnitTable WithUserUnits(const Value &units);

	const UnitDef *Find(const string &token) const;
	//! The longest alias that is a prefix of text, or nullptr; matched is its length.
	const UnitDef *LongestPrefix(const string &text, idx_t &matched) const;
	string BaseUnit(const string &dimension) const;
	const vector<UnitDef> &Units() const {
		return units;
	}

private:
	void Add(UnitDef unit);
	void Index();
	vector<UnitDef> units;
	std::unordered_map<string, idx_t> by_alias;       // exact
	std::unordered_map<string, idx_t> by_lower_alias; // words of 3+ letters, case-insensitive
	std::unordered_map<string, string> base_units;
	idx_t max_alias_length = 0;
};

//! A parsed quantity. lo/hi are in the base unit of the dimension.
struct Quantity {
	bool has_value = false;
	double lo = 0;
	double hi = 0;
	string unit;        // base unit (or the source unit when unknown)
	string dimension;   // empty: unknown
	string bound;       // at_least | at_most | within | equals | empty (unqualified)
	string source_unit; // as written
	string category;    // ordinal class such as IP54
};

Quantity ParseQuantity(const string &text, const vector<string> &langs, const UnitTable &units);

//! Result of a direction-aware comparison.
struct ValueScore {
	double score = 0;
	bool has_distance = false;
	double distance = 0;
	string rule; // at_least | at_most | within | equals | point | gate | missing
};

struct ValueSide {
	bool has_lo = false;
	bool has_hi = false;
	double lo = 0;
	double hi = 0;
	string dimension;
	string unit;
	string bound;
	bool unit_gate = false; // the unit is unknown or belongs to another dimension
};

struct ValueOptions {
	string scale; // empty: per dimension
	double tol = 0.02;
	double soft_band = 0.05;
	string default_bound;
};

ValueScore ValueSatisfies(const ValueSide &req, const ValueSide &prov, const ValueOptions &options);

void RegisterQuantityFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

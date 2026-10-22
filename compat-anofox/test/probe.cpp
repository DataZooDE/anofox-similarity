#include "duckdb.hpp"
#include "duckdb_compat/struct_vector.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "compat_anofox/macro_doc.hpp"

static duckdb::unique_ptr<duckdb::FunctionData> SideEffect() {
	return nullptr;
}

int main() {
	auto callback = compat_anofox::ScalarBind<&SideEffect>();
	duckdb::Vector struct_vector(duckdb::LogicalType::STRUCT({{"x", duckdb::LogicalType::INTEGER}}));
	auto &children = duckdb::StructVector::GetEntries(struct_vector);
	(void)duckdb_compat::StructChild(children[0]);
	return callback == nullptr;
}

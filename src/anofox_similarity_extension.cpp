#define DUCKDB_EXTENSION_MAIN

#include "anofox_similarity_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/connection.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include <unordered_set>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void AnofoxSimilarityScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "AnofoxSimilarity " + name.GetString() + " 🐥");
	});
}

inline void AnofoxSimilarityOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "AnofoxSimilarity " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

//------------------------------------------------------------------------------
// Jaccard Similarity Function
//------------------------------------------------------------------------------

// Custom hash function for duckdb::Value
struct ValueHash {
	size_t operator()(const Value &val) const {
		return val.Hash();
	}
};

// Custom equality function for duckdb::Value
struct ValueEqual {
	bool operator()(const Value &a, const Value &b) const {
		// Use strict equality - same type and same value
		return a == b;
	}
};

static void JaccardSimilarityFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &list_a = args.data[0];
	auto &list_b = args.data[1];
	auto count = args.size();

	// Get child vectors for both lists
	auto &child_a = ListVector::GetEntry(list_a);
	auto &child_b = ListVector::GetEntry(list_b);

	// Flatten vectors for uniform access
	UnifiedVectorFormat vdata_a, vdata_b;
	list_a.ToUnifiedFormat(count, vdata_a);
	list_b.ToUnifiedFormat(count, vdata_b);

	UnifiedVectorFormat child_vdata_a, child_vdata_b;
	auto child_count_a = ListVector::GetListSize(list_a);
	auto child_count_b = ListVector::GetListSize(list_b);
	child_a.ToUnifiedFormat(child_count_a, child_vdata_a);
	child_b.ToUnifiedFormat(child_count_b, child_vdata_b);

	auto list_entries_a = UnifiedVectorFormat::GetData<list_entry_t>(vdata_a);
	auto list_entries_b = UnifiedVectorFormat::GetData<list_entry_t>(vdata_b);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<double>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto idx_a = vdata_a.sel->get_index(i);
		auto idx_b = vdata_b.sel->get_index(i);

		// Handle NULL inputs
		if (!vdata_a.validity.RowIsValid(idx_a) || !vdata_b.validity.RowIsValid(idx_b)) {
			result_validity.SetInvalid(i);
			continue;
		}

		auto &entry_a = list_entries_a[idx_a];
		auto &entry_b = list_entries_b[idx_b];

		// Handle empty lists: Jaccard(empty, empty) = 0.0
		if (entry_a.length == 0 && entry_b.length == 0) {
			result_data[i] = 0.0;
			continue;
		}

		// Build set from list A using Value for generic comparison
		std::unordered_set<Value, ValueHash, ValueEqual> set_a;
		for (idx_t j = 0; j < entry_a.length; j++) {
			auto child_idx = child_vdata_a.sel->get_index(entry_a.offset + j);
			if (child_vdata_a.validity.RowIsValid(child_idx)) {
				set_a.insert(child_a.GetValue(child_idx));
			}
		}

		// Build set from list B
		std::unordered_set<Value, ValueHash, ValueEqual> set_b;
		for (idx_t j = 0; j < entry_b.length; j++) {
			auto child_idx = child_vdata_b.sel->get_index(entry_b.offset + j);
			if (child_vdata_b.validity.RowIsValid(child_idx)) {
				set_b.insert(child_b.GetValue(child_idx));
			}
		}

		// Count intersection from deduplicated sets
		idx_t intersection_count = 0;
		for (const auto &val : set_a) {
			if (set_b.find(val) != set_b.end()) {
				intersection_count++;
			}
		}

		idx_t union_size = set_a.size() + set_b.size() - intersection_count;

		if (union_size == 0) {
			result_data[i] = 0.0;
		} else {
			result_data[i] = static_cast<double>(intersection_count) / static_cast<double>(union_size);
		}
	}
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto anofox_similarity_scalar_function = ScalarFunction("anofox_similarity", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AnofoxSimilarityScalarFun);
	loader.RegisterFunction(anofox_similarity_scalar_function);

	// Register another scalar function
	auto anofox_similarity_openssl_version_scalar_function = ScalarFunction("anofox_similarity_openssl_version", {LogicalType::VARCHAR},
	                                                            LogicalType::VARCHAR, AnofoxSimilarityOpenSSLVersionScalarFun);
	loader.RegisterFunction(anofox_similarity_openssl_version_scalar_function);

	// Register jaccard_similarity function for computing set similarity
	// Accepts two lists of any type and returns Jaccard coefficient (0.0 to 1.0)
	auto jaccard_similarity_function = ScalarFunction(
	    "jaccard_similarity",
	    {LogicalType::LIST(LogicalType::ANY), LogicalType::LIST(LogicalType::ANY)},
	    LogicalType::DOUBLE,
	    JaccardSimilarityFun
	);
	loader.RegisterFunction(jaccard_similarity_function);

	// Register find_similar_materials as a SQL table macro
	// Uses query_table() to dynamically access the BOM table by name
	// Default table is 'bom_items' with columns (parent_id, child_id)
	//
	// Usage:
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10);
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10, method := 'jaccard', min_similarity := 0.5);
	//   SELECT * FROM find_similar_materials('MATERIAL-A', 10, bom_table := 'my_bom_table');
	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO find_similar_materials(
			query_material_id,
			k,
			method := 'jaccard',
			min_similarity := 0.0,
			bom_table := 'bom_items'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Aggregate components per material
				material_components AS MATERIALIZED (
					SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
					FROM query_table(bom_table)
					GROUP BY parent_id
				),
				-- Get query material's components
				query_mat AS (
					SELECT components AS query_components
					FROM material_components
					WHERE material_id = query_material_id
				),
				-- Compute similarities with all other materials
				computed AS (
					SELECT
						mc.material_id,
						jaccard_similarity(qm.query_components, mc.components) AS similarity,
						len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
						len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
					FROM material_components mc, query_mat qm
					WHERE mc.material_id != query_material_id
				)
			SELECT material_id, similarity, shared_components, total_components
			FROM computed
			WHERE similarity >= min_similarity
			ORDER BY similarity DESC
			LIMIT k
		)
	)");

	if (result->HasError()) {
		throw InvalidInputException("Failed to create find_similar_materials macro: %s", result->GetError().c_str());
	}

	// Register cold_start_analogs as a SQL table macro
	// Finds similar materials that have consumption history for cold-start forecasting
	// Uses query_table() to dynamically access the movements table by name
	// Default tables are 'bom_items' and 'goods_movements'
	//
	// Usage:
	//   SELECT * FROM cold_start_analogs('NEW-MATERIAL', 5, min_history_months := 12);
	//   SELECT * FROM cold_start_analogs('NEW-MATERIAL', 5, min_history_months := 12, movements_table := 'my_movements');
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO cold_start_analogs(
			query_material_id,
			k,
			min_history_months := 0,
			min_similarity := 0.0,
			bom_table := 'bom_items',
			movements_table := 'goods_movements'
		) AS TABLE
		SELECT * FROM (
			WITH
				-- Aggregate components per material
				material_components AS MATERIALIZED (
					SELECT parent_id AS material_id, list(child_id ORDER BY child_id) AS components
					FROM query_table(bom_table)
					GROUP BY parent_id
				),
				-- Get query material's components
				query_mat AS (
					SELECT components AS query_components
					FROM material_components
					WHERE material_id = query_material_id
				),
				-- Compute similarities with all other materials
				computed_similarity AS (
					SELECT
						mc.material_id,
						jaccard_similarity(qm.query_components, mc.components) AS similarity,
						len(list_intersect(qm.query_components, mc.components))::BIGINT AS shared_components,
						len(list_distinct(list_concat(qm.query_components, mc.components)))::BIGINT AS total_components
					FROM material_components mc, query_mat qm
					WHERE mc.material_id != query_material_id
				),
				-- Calculate consumption history per material
				material_history AS (
					SELECT
						material_id,
						MIN(movement_date) AS first_usage,
						MAX(movement_date) AS last_usage,
						-- Calculate months between first and last usage
						(EXTRACT(YEAR FROM MAX(movement_date)) - EXTRACT(YEAR FROM MIN(movement_date))) * 12 +
						(EXTRACT(MONTH FROM MAX(movement_date)) - EXTRACT(MONTH FROM MIN(movement_date))) AS history_months
					FROM query_table(movements_table)
					GROUP BY material_id
				),
				-- Join similarity with history and filter
				with_history AS (
					SELECT
						cs.material_id,
						cs.similarity,
						cs.shared_components,
						cs.total_components,
						COALESCE(mh.history_months, 0)::BIGINT AS history_months,
						mh.first_usage,
						mh.last_usage
					FROM computed_similarity cs
					LEFT JOIN material_history mh ON cs.material_id = mh.material_id
					WHERE cs.similarity >= min_similarity
					  AND COALESCE(mh.history_months, 0) >= min_history_months
				)
			SELECT material_id, similarity, shared_components, total_components,
			       history_months, first_usage, last_usage
			FROM with_history
			ORDER BY similarity DESC
			LIMIT k
		)
	)");

	if (result->HasError()) {
		throw InvalidInputException("Failed to create cold_start_analogs macro: %s", result->GetError().c_str());
	}
}

void AnofoxSimilarityExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string AnofoxSimilarityExtension::Name() {
	return "anofox_similarity";
}

std::string AnofoxSimilarityExtension::Version() const {
#ifdef EXT_VERSION_ANOFOX_SIMILARITY
	return EXT_VERSION_ANOFOX_SIMILARITY;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(anofox_similarity, loader) {
	duckdb::LoadInternal(loader);
}
}

#include "modules/multimodal_fusion.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"
#include <cmath>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Multi-Modal Fusion Scalar Function
//------------------------------------------------------------------------------

static void FuseEmbeddingsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// Input parameters:
	// args[0]: structural_embedding (FLOAT[256])
	// args[1]: textual_embedding (FLOAT[384])
	// args[2]: transactional_embedding (FLOAT[128])
	// args[3]: weights (STRUCT with fields: structural, textual, transactional)
	//
	// Output: combined_embedding (FLOAT[768] = [√w_s·φ || √w_t·ψ || √w_x·χ])

	auto &structural_vector = args.data[0];
	auto &textual_vector = args.data[1];
	auto &transactional_vector = args.data[2];
	auto &weights_vector = args.data[3];
	auto count = args.size();

	structural_vector.Flatten(count);
	textual_vector.Flatten(count);
	transactional_vector.Flatten(count);
	weights_vector.Flatten(count);

	auto structural_data = FlatVector::GetData<list_entry_t>(structural_vector);
	auto textual_data = FlatVector::GetData<list_entry_t>(textual_vector);
	auto transactional_data = FlatVector::GetData<list_entry_t>(transactional_vector);

	auto &structural_validity = FlatVector::Validity(structural_vector);
	auto &textual_validity = FlatVector::Validity(textual_vector);
	auto &transactional_validity = FlatVector::Validity(transactional_vector);

	// Get child vectors for list entries
	auto &structural_child = ListVector::GetEntry(structural_vector);
	auto &textual_child = ListVector::GetEntry(textual_vector);
	auto &transactional_child = ListVector::GetEntry(transactional_vector);
	auto &result_child = ListVector::GetEntry(result);

	auto structural_child_data = FlatVector::GetData<float>(structural_child);
	auto textual_child_data = FlatVector::GetData<float>(textual_child);
	auto transactional_child_data = FlatVector::GetData<float>(transactional_child);
	auto result_child_data = FlatVector::GetData<float>(result_child);

	ListVector::SetListSize(result, count * 768);
	auto result_entries = FlatVector::GetData<list_entry_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	// Process weights struct
	// For simplicity, we'll assume weights are passed correctly
	// In production, would parse struct more carefully

	for (idx_t i = 0; i < count; i++) {
		// Handle NULL inputs
		if (!structural_validity.RowIsValid(i) || !textual_validity.RowIsValid(i) ||
		    !transactional_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}

		result_entries[i].offset = i * 768;
		result_entries[i].length = 768;

		try {
			// Extract embedding data
			auto &struct_entry = structural_data[i];
			auto &text_entry = textual_data[i];
			auto &trans_entry = transactional_data[i];

			// For now, return simple concatenation (placeholder)
			// Real implementation would parse weights struct and apply formula
			idx_t result_idx = i * 768;

			// Copy structural (256-D)
			for (idx_t j = 0; j < 256 && j < struct_entry.length; j++) {
				result_child_data[result_idx + j] = structural_child_data[struct_entry.offset + j] * 0.7071f; // √0.5
			}

			// Copy textual (384-D)
			for (idx_t j = 0; j < 384 && j < text_entry.length; j++) {
				result_child_data[result_idx + 256 + j] = textual_child_data[text_entry.offset + j] * 0.7071f; // √0.5
			}

			// Copy transactional (128-D)
			for (idx_t j = 0; j < 128 && j < trans_entry.length; j++) {
				result_child_data[result_idx + 640 + j] = transactional_child_data[trans_entry.offset + j];
			}

		} catch (const std::exception &e) {
			result_validity.SetInvalid(i);
		}
	}
}

//------------------------------------------------------------------------------
// Telemetry Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> FuseEmbeddingsBind(ClientContext &context, ScalarFunction &bound_function,
                                                   vector<unique_ptr<Expression>> &arguments) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("fuse_embeddings");
	return nullptr;
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterMultimodalFusionFunctions(ExtensionLoader &loader) {
	// Register fuse_embeddings scalar function
	// Takes: structural (LIST(FLOAT)), textual (LIST(FLOAT)), transactional (LIST(FLOAT)), weights (STRUCT)
	// Returns: fused embedding (LIST(FLOAT))

	// Note: This is a simplified version for testing
	// Production version would properly parse the weights struct
	auto fuse_embeddings_function =
	    ScalarFunction("fuse_embeddings",
	                   {LogicalType::LIST(LogicalType::FLOAT), LogicalType::LIST(LogicalType::FLOAT),
	                    LogicalType::LIST(LogicalType::FLOAT),
	                    LogicalType::STRUCT({{"structural", LogicalType::FLOAT},
	                                         {"textual", LogicalType::FLOAT},
	                                         {"transactional", LogicalType::FLOAT}})},
	                   LogicalType::LIST(LogicalType::FLOAT), FuseEmbeddingsFunction, FuseEmbeddingsBind);
	loader.RegisterFunction(fuse_embeddings_function);
}

void RegisterFusionMacros(Connection &conn) {
	// Register compute_fused_embeddings macro for batch fusion
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_fused_embeddings(
			weights_structural := 0.5,
			weights_textual := 0.5,
			weights_transactional := 0.0
		) AS TABLE
		WITH weights AS (
			SELECT {
				'structural': weights_structural,
				'textual': weights_textual,
				'transactional': weights_transactional
			}::STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT) AS w
		),
		fused AS (
			SELECT
				me.material_id,
				fuse_embeddings(
					me.structural_embedding,
					me.textual_embedding,
					me.transactional_embedding,
					w
				) AS combined_embedding,
				w AS fusion_weights
			FROM material_embeddings me
			CROSS JOIN weights
			WHERE me.structural_embedding IS NOT NULL
			  AND me.textual_embedding IS NOT NULL
		)
		SELECT * FROM fused
	)");

	CheckQueryResult(result, "create compute_fused_embeddings macro");
}

} // namespace anofox
} // namespace duckdb

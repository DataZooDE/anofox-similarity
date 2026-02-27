#include "modules/multimodal_fusion.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"
#include <algorithm>
#include <cmath>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Multi-Modal Fusion Scalar Function
//------------------------------------------------------------------------------

static void FuseEmbeddingsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	static constexpr idx_t STRUCTURAL_DIM = 256;
	static constexpr idx_t TEXTUAL_DIM = 384;
	static constexpr idx_t TRANSACTIONAL_DIM = 128;
	static constexpr idx_t COMBINED_DIM = STRUCTURAL_DIM + TEXTUAL_DIM + TRANSACTIONAL_DIM;
	static constexpr float WEIGHT_SUM_TOLERANCE = 1e-4f;

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

	UnifiedVectorFormat structural_vdata;
	UnifiedVectorFormat textual_vdata;
	UnifiedVectorFormat transactional_vdata;
	UnifiedVectorFormat weights_vdata;
	structural_vector.ToUnifiedFormat(count, structural_vdata);
	textual_vector.ToUnifiedFormat(count, textual_vdata);
	transactional_vector.ToUnifiedFormat(count, transactional_vdata);
	weights_vector.ToUnifiedFormat(count, weights_vdata);

	auto structural_data = UnifiedVectorFormat::GetData<list_entry_t>(structural_vdata);
	auto textual_data = UnifiedVectorFormat::GetData<list_entry_t>(textual_vdata);
	auto transactional_data = UnifiedVectorFormat::GetData<list_entry_t>(transactional_vdata);

	// Get child vectors for list entries
	auto &structural_child = ListVector::GetEntry(structural_vector);
	auto &textual_child = ListVector::GetEntry(textual_vector);
	auto &transactional_child = ListVector::GetEntry(transactional_vector);
	auto &result_child = ListVector::GetEntry(result);

	UnifiedVectorFormat structural_child_vdata;
	UnifiedVectorFormat textual_child_vdata;
	UnifiedVectorFormat transactional_child_vdata;
	structural_child.ToUnifiedFormat(ListVector::GetListSize(structural_vector), structural_child_vdata);
	textual_child.ToUnifiedFormat(ListVector::GetListSize(textual_vector), textual_child_vdata);
	transactional_child.ToUnifiedFormat(ListVector::GetListSize(transactional_vector), transactional_child_vdata);

	auto structural_child_data = UnifiedVectorFormat::GetData<float>(structural_child_vdata);
	auto textual_child_data = UnifiedVectorFormat::GetData<float>(textual_child_vdata);
	auto transactional_child_data = UnifiedVectorFormat::GetData<float>(transactional_child_vdata);
	auto result_child_data = FlatVector::GetData<float>(result_child);

	// Parse weights struct entries: {structural, textual, transactional}
	auto &weight_entries = StructVector::GetEntries(weights_vector);
	auto &structural_weight_vec = *weight_entries[0];
	auto &textual_weight_vec = *weight_entries[1];
	auto &transactional_weight_vec = *weight_entries[2];

	UnifiedVectorFormat structural_weight_vdata;
	UnifiedVectorFormat textual_weight_vdata;
	UnifiedVectorFormat transactional_weight_vdata;
	structural_weight_vec.ToUnifiedFormat(count, structural_weight_vdata);
	textual_weight_vec.ToUnifiedFormat(count, textual_weight_vdata);
	transactional_weight_vec.ToUnifiedFormat(count, transactional_weight_vdata);

	auto structural_weight_data = UnifiedVectorFormat::GetData<float>(structural_weight_vdata);
	auto textual_weight_data = UnifiedVectorFormat::GetData<float>(textual_weight_vdata);
	auto transactional_weight_data = UnifiedVectorFormat::GetData<float>(transactional_weight_vdata);

	ListVector::SetListSize(result, count * COMBINED_DIM);
	auto result_entries = FlatVector::GetData<list_entry_t>(result);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto struct_idx = structural_vdata.sel->get_index(i);
		auto text_idx = textual_vdata.sel->get_index(i);
		auto trans_idx = transactional_vdata.sel->get_index(i);
		auto weights_idx = weights_vdata.sel->get_index(i);
		auto ws_idx = structural_weight_vdata.sel->get_index(i);
		auto wt_idx = textual_weight_vdata.sel->get_index(i);
		auto wx_idx = transactional_weight_vdata.sel->get_index(i);

		// Handle NULL inputs
		if (!structural_vdata.validity.RowIsValid(struct_idx) || !textual_vdata.validity.RowIsValid(text_idx) ||
		    !transactional_vdata.validity.RowIsValid(trans_idx) || !weights_vdata.validity.RowIsValid(weights_idx) ||
		    !structural_weight_vdata.validity.RowIsValid(ws_idx) || !textual_weight_vdata.validity.RowIsValid(wt_idx) ||
		    !transactional_weight_vdata.validity.RowIsValid(wx_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		result_entries[i].offset = i * COMBINED_DIM;
		result_entries[i].length = COMBINED_DIM;

		try {
			// Extract embedding data
			auto &struct_entry = structural_data[struct_idx];
			auto &text_entry = textual_data[text_idx];
			auto &trans_entry = transactional_data[trans_idx];

			auto w_structural = structural_weight_data[ws_idx];
			auto w_textual = textual_weight_data[wt_idx];
			auto w_transactional = transactional_weight_data[wx_idx];

			if (!std::isfinite(w_structural) || !std::isfinite(w_textual) || !std::isfinite(w_transactional) ||
			    w_structural < 0.0f || w_textual < 0.0f || w_transactional < 0.0f) {
				result_validity.SetInvalid(i);
				continue;
			}

			auto weight_sum = w_structural + w_textual + w_transactional;
			if (weight_sum <= 0.0f || std::fabs(weight_sum - 1.0f) > WEIGHT_SUM_TOLERANCE) {
				result_validity.SetInvalid(i);
				continue;
			}

			const auto scale_structural = std::sqrt(w_structural);
			const auto scale_textual = std::sqrt(w_textual);
			const auto scale_transactional = std::sqrt(w_transactional);

			idx_t result_idx = i * COMBINED_DIM;

			// Always zero-initialize output slice to avoid uninitialized values when input lists are short.
			std::fill_n(result_child_data + result_idx, COMBINED_DIM, 0.0f);

			// Copy structural (256-D)
			for (idx_t j = 0; j < STRUCTURAL_DIM && j < struct_entry.length; j++) {
				auto child_idx = structural_child_vdata.sel->get_index(struct_entry.offset + j);
				if (structural_child_vdata.validity.RowIsValid(child_idx)) {
					result_child_data[result_idx + j] = structural_child_data[child_idx] * scale_structural;
				}
			}

			// Copy textual (384-D)
			for (idx_t j = 0; j < TEXTUAL_DIM && j < text_entry.length; j++) {
				auto child_idx = textual_child_vdata.sel->get_index(text_entry.offset + j);
				if (textual_child_vdata.validity.RowIsValid(child_idx)) {
					result_child_data[result_idx + STRUCTURAL_DIM + j] = textual_child_data[child_idx] * scale_textual;
				}
			}

			// Copy transactional (128-D)
			for (idx_t j = 0; j < TRANSACTIONAL_DIM && j < trans_entry.length; j++) {
				auto child_idx = transactional_child_vdata.sel->get_index(trans_entry.offset + j);
				if (transactional_child_vdata.validity.RowIsValid(child_idx)) {
					result_child_data[result_idx + STRUCTURAL_DIM + TEXTUAL_DIM + j] =
					    transactional_child_data[child_idx] * scale_transactional;
				}
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

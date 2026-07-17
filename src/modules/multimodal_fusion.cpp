#include "modules/multimodal_fusion.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
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
	// Reserve the result child vector to hold count * COMBINED_DIM floats BEFORE taking a pointer
	// into it. The list child starts at STANDARD_VECTOR_SIZE capacity; without this reserve, any
	// batch whose count * 768 exceeds that capacity overflowed the heap (SIGSEGV). Reserve may
	// reallocate, so result_child_data is fetched afterwards (below).
	ListVector::Reserve(result, count * COMBINED_DIM);
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

		// The weights struct itself must be present; individual modality vectors may be NULL
		// as long as their weight is zero (a zero-weighted modality is treated as absent).
		if (!weights_vdata.validity.RowIsValid(weights_idx) ||
		    !structural_weight_vdata.validity.RowIsValid(ws_idx) || !textual_weight_vdata.validity.RowIsValid(wt_idx) ||
		    !transactional_weight_vdata.validity.RowIsValid(wx_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		result_entries[i].offset = i * COMBINED_DIM;
		result_entries[i].length = COMBINED_DIM;

		try {
			auto w_structural = structural_weight_data[ws_idx];
			auto w_textual = textual_weight_data[wt_idx];
			auto w_transactional = transactional_weight_data[wx_idx];

			if (!std::isfinite(w_structural) || !std::isfinite(w_textual) || !std::isfinite(w_transactional) ||
			    w_structural < 0.0f || w_textual < 0.0f || w_transactional < 0.0f) {
				result_validity.SetInvalid(i);
				continue;
			}

			// A modality only contributes when its weight is strictly positive. When it does
			// contribute, its embedding vector must be present; a NULL vector at a positive
			// weight makes the fused result NULL. At weight 0 the vector is ignored, so a
			// missing (NULL) transactional vector no longer nulls out the whole fusion.
			const bool use_structural = w_structural > 0.0f;
			const bool use_textual = w_textual > 0.0f;
			const bool use_transactional = w_transactional > 0.0f;

			if ((use_structural && !structural_vdata.validity.RowIsValid(struct_idx)) ||
			    (use_textual && !textual_vdata.validity.RowIsValid(text_idx)) ||
			    (use_transactional && !transactional_vdata.validity.RowIsValid(trans_idx))) {
				result_validity.SetInvalid(i);
				continue;
			}

			auto weight_sum = w_structural + w_textual + w_transactional;
			if (weight_sum <= 0.0f) {
				result_validity.SetInvalid(i);
				continue;
			}
			// Normalize the active weights so they sum to 1, instead of silently returning NULL
			// when the caller's weights do not. This matches the documented behaviour
			// ("weights do not need to sum to 1.0") and keeps the sqrt-weighted concatenation stable.
			w_structural /= weight_sum;
			w_textual /= weight_sum;
			w_transactional /= weight_sum;

			auto struct_entry = use_structural ? structural_data[struct_idx] : list_entry_t {0, 0};
			auto text_entry = use_textual ? textual_data[text_idx] : list_entry_t {0, 0};
			auto trans_entry = use_transactional ? transactional_data[trans_idx] : list_entry_t {0, 0};

			// Validate dimensions of every contributing modality instead of silently zero-padding a
			// short vector or truncating a long one — a wrong-length embedding is a caller bug, so
			// the fused result for that row is NULL rather than a quietly-corrupted vector.
			if ((use_structural && struct_entry.length != STRUCTURAL_DIM) ||
			    (use_textual && text_entry.length != TEXTUAL_DIM) ||
			    (use_transactional && trans_entry.length != TRANSACTIONAL_DIM)) {
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
	PostHogTelemetry::Instance().RecordFunctionCall("fuse_embeddings");
	return nullptr;
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterMultimodalFusionFunctions(ExtensionLoader &loader) {
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
	// SPECIAL_HANDLING so the function is invoked even when a modality vector is NULL; the body
	// decides per-modality whether a NULL is acceptable (it is, when that modality's weight is 0).
	// With default handling DuckDB would short-circuit the whole result to NULL, which is why a
	// NULL transactional vector used to null out compute_fused_embeddings on its default weights.
	fuse_embeddings_function.null_handling = FunctionNullHandling::SPECIAL_HANDLING;

	CreateScalarFunctionInfo info(fuse_embeddings_function);
	FunctionDescription desc;
	desc.description = "Fuses structural (FLOAT[256]), textual (FLOAT[384]), and transactional (FLOAT[128]) "
	                   "embeddings into a single 768-dimensional combined embedding using weighted concatenation. "
	                   "Each component is scaled by sqrt(weight). Weights do not need to sum to 1.0.";
	desc.examples    = {"SELECT fuse_embeddings(se, te, xe, {'structural':0.5,'textual':0.5,'transactional':0.0}) "
	                    "FROM material_embeddings;"};
	desc.categories  = {"embeddings", "fusion"};
	desc.parameter_names = {"structural_embedding", "textual_embedding", "transactional_embedding", "weights"};
	desc.parameter_types = {LogicalType::LIST(LogicalType::FLOAT),
	                         LogicalType::LIST(LogicalType::FLOAT),
	                         LogicalType::LIST(LogicalType::FLOAT),
	                         LogicalType::STRUCT({{"structural", LogicalType::FLOAT},
	                                              {"textual", LogicalType::FLOAT},
	                                              {"transactional", LogicalType::FLOAT}})};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterFusionMacros(Connection &conn) {
	// Register compute_fused_embeddings macro for batch fusion
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_fused_embeddings(
			weights_structural := 0.5,
			weights_textual := 0.5,
			weights_transactional := 0.0
		) AS TABLE
		WITH raw_weights AS (
			-- COALESCE each weight to its documented default so an explicit NULL falls back instead of
			-- poisoning the whole computation (a NULL operand would make the normalized weights NULL
			-- and every fused embedding NULL).
			SELECT COALESCE(weights_structural, 0.5) AS ws,
			       COALESCE(weights_textual, 0.5) AS wt,
			       COALESCE(weights_transactional, 0.0) AS wx
		),
		weights AS (
			-- Normalize so fusion_weights reflects the weights actually applied by fuse_embeddings
			-- (which normalizes internally); guard against a zero sum.
			SELECT {
				'structural': ws / NULLIF(ws + wt + wx, 0),
				'textual': wt / NULLIF(ws + wt + wx, 0),
				'transactional': wx / NULLIF(ws + wt + wx, 0)
			}::STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT) AS w
			FROM raw_weights
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
			CROSS JOIN raw_weights
			-- Only require a modality's vector when that modality has positive weight, mirroring
			-- fuse_embeddings itself. Gate on the COALESCEd raw_weights.ws/wt/wx (not the bare
			-- weights_structural/weights_textual/weights_transactional macro params): an explicit
			-- weights_transactional := NULL must be treated as its default 0.0, not as
			-- "NULL = 0 is NULL" (falsy), which previously dropped every row lacking that modality.
			WHERE (ws = 0 OR me.structural_embedding IS NOT NULL)
			  AND (wt = 0 OR me.textual_embedding IS NOT NULL)
			  AND (wx = 0 OR me.transactional_embedding IS NOT NULL)
		)
		SELECT * FROM fused
	)");

	CheckQueryResult(result, "create compute_fused_embeddings macro");
}

} // namespace anofox
} // namespace duckdb

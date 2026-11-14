#include "modules/textual_embeddings.hpp"
#include "duckdb_compat.hpp"
#include "duckdb_compat/list_vector.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "compat_anofox/array_vector.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/types/vector.hpp"
#include "telemetry.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "datazoo_function_doc.hpp"
#include "compat_anofox/macro_doc.hpp"

namespace ddoc = datazoo::doc;

#ifdef DUCKDB_OPENVINO_AVAILABLE
#include "openvino/openvino.hpp"
#endif

#include <memory>
#include <map>
#include "anofox_similarity_banner.hpp"


namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Placeholder vectors
//------------------------------------------------------------------------------

// A deterministic stand-in for a text model: element i is a 64-bit mix of (FNV-1a of the text, i)
// mapped to [-0.5, 0.5). Identical on every platform and compiler (std::hash is not), and
// distinct texts do not collide in practice. It carries no meaning: similar texts do not get
// similar vectors.
static vector<float> PlaceholderEmbedding(const string &text, idx_t dimension) {
	vector<float> embedding(dimension, 0.0f);
	if (text.empty()) {
		return embedding;
	}
	uint64_t h = 1469598103934665603ULL; // FNV-1a 64
	for (unsigned char c : text) {
		h = (h ^ c) * 1099511628211ULL;
	}
	for (idx_t i = 0; i < dimension; i++) {
		uint64_t z = h + 0x9E3779B97F4A7C15ULL * (i + 1); // SplitMix64
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		z ^= z >> 31;
		embedding[i] = static_cast<float>(static_cast<double>(z >> 11) / 9007199254740992.0 - 0.5);
	}
	return embedding;
}


//------------------------------------------------------------------------------
// Embedding Provider Base Class
//------------------------------------------------------------------------------

class EmbeddingProvider {
public:
	virtual ~EmbeddingProvider() = default;
	virtual vector<float> Embed(const string &text) = 0;
	virtual idx_t Dimension() const = 0;
};

//------------------------------------------------------------------------------
// Gemma Local Provider (OpenVINO-based)
//------------------------------------------------------------------------------

#ifdef DUCKDB_OPENVINO_AVAILABLE

class GemmaLocalProvider : public EmbeddingProvider {
private:
	std::unique_ptr<ov::Core> core;
	std::unique_ptr<ov::CompiledModel> compiled_model;
	string model_path;
	bool initialized;

public:
	GemmaLocalProvider(const string &path = "") : model_path(path), initialized(false) {
		// Note: Full Gemma model initialization would go here
		// For now, this is a placeholder that returns dummy embeddings
		// Actual implementation requires:
		// 1. Model path detection/validation
		// 2. OpenVINO model loading
		// 3. Input/output shape configuration
		// 4. Text tokenization pipeline
	}

	vector<float> Embed(const string &text) override {
		// Placeholder: return 384-D zero vector
		// Real implementation would:
		// 1. Tokenize text
		// 2. Run inference
		// 3. Extract pooled embeddings
		return PlaceholderEmbedding(text, 384);
	}

	idx_t Dimension() const override {
		return 384;
	}
};

#endif

//------------------------------------------------------------------------------
// HTTP API Embedding Provider (OpenAI, Anthropic, etc.)
//------------------------------------------------------------------------------

class HTTPEmbeddingProvider : public EmbeddingProvider {
private:
	string endpoint;
	string api_key;
	string provider_name;
	idx_t dimension;

public:
	HTTPEmbeddingProvider(const string &provider, const string &key, const string &ep = "", idx_t dim = 384)
	    : provider_name(provider), api_key(key), endpoint(ep), dimension(dim) {
		// Configure provider-specific endpoints
		if (provider == "openai") {
			if (endpoint.empty()) {
				endpoint = "https://api.openai.com/v1/embeddings";
			}
			dimension = 1536; // OpenAI's embedding dimension
		} else if (provider == "anthropic") {
			if (endpoint.empty()) {
				endpoint = "https://api.anthropic.com/v1/embeddings";
			}
			dimension = 1024;
		} else if (provider == "cohere") {
			if (endpoint.empty()) {
				endpoint = "https://api.cohere.com/v1/embed";
			}
			dimension = 384;
		}
	}

	vector<float> Embed(const string &text) override {
		// Placeholder: In production, this would make HTTP requests
		// For now, return deterministic embeddings based on text
		return PlaceholderEmbedding(text, dimension);
	}

	idx_t Dimension() const override {
		return dimension;
	}
};

//------------------------------------------------------------------------------
// Provider Factory
//------------------------------------------------------------------------------

// Every provider is a stateless placeholder, so there is nothing to cache and provider_config is
// not read (no secrets are parsed or kept). A real backend should take its key from a DuckDB secret.
class EmbeddingProviderFactory {
public:
	// Provider names are matched case-insensitively, ignoring surrounding blanks.
	static string Normalize(const string &provider_name) {
		auto name = StringUtil::Lower(provider_name);
		StringUtil::Trim(name);
		return name;
	}

	static bool IsKnown(const string &name) {
		return name == "gemma-local" || name == "openai" || name == "anthropic" || name == "cohere";
	}

	static std::unique_ptr<EmbeddingProvider> GetProvider(const string &name) {
#ifdef DUCKDB_OPENVINO_AVAILABLE
		if (name == "gemma-local") {
			return std::unique_ptr<EmbeddingProvider>(new GemmaLocalProvider());
		}
#endif
		// gemma-local without OpenVINO uses the placeholder of the HTTP providers
		return std::unique_ptr<EmbeddingProvider>(new HTTPEmbeddingProvider(name == "gemma-local" ? "openai" : name, ""));
	}
};

//------------------------------------------------------------------------------
// Embedding Backend Scalar Function
//------------------------------------------------------------------------------

static void EmbeddingBackendFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &text_vector = args.data[0];
	auto &provider_vector = args.data[1];
	auto count = args.size();

	text_vector.Flatten(count);
	provider_vector.Flatten(count);

	auto text_data = FlatVector::GetData<string_t>(text_vector);
	auto provider_data = FlatVector::GetData<string_t>(provider_vector);

	auto &text_validity = FlatVector::Validity(text_vector);
	auto &provider_validity = FlatVector::Validity(provider_vector);

	// FLOAT[384] (ARRAY): the child vector of an ARRAY result already holds count * 384 floats,
	// row i at [i * 384, (i + 1) * 384).
	auto &child = compat_anofox::ArrayChild(result);
	auto child_data = duckdb_compat::FlatData<float>(child);
	auto &result_validity = duckdb_compat::MutableValidity(result);

	// Fill embeddings
	for (idx_t i = 0; i < count; i++) {
		// Handle NULL inputs
		if (!text_validity.RowIsValid(i) || !provider_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}
		string provider_name = EmbeddingProviderFactory::Normalize(provider_data[i].GetString());
		if (!EmbeddingProviderFactory::IsKnown(provider_name)) {
			throw InvalidInputException("embedding_backend: unknown provider '%s' (expected gemma-local, openai, "
			                            "anthropic or cohere)",
			                            provider_name);
		}
		for (idx_t j = 0; j < 384; j++) {
			child_data[i * 384 + j] = 0.0f;
		}

		try {
			string text = text_data[i].GetString();
			auto provider = EmbeddingProviderFactory::GetProvider(provider_name);
			auto embedding = provider->Embed(text);

			// Copy embedding to result
			for (idx_t j = 0; j < 384 && j < embedding.size(); j++) {
				child_data[i * 384 + j] = embedding[j];
			}
		} catch (const std::exception &e) {
			result_validity.SetInvalid(i);
		}
	}
}

//------------------------------------------------------------------------------
// Telemetry Bind Function
//------------------------------------------------------------------------------

static unique_ptr<FunctionData> EmbeddingBackendBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("embedding_backend");
	return nullptr;
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterTextualEmbeddingFunctions(ExtensionLoader &loader) {
	// Returns FLOAT[384] (ARRAY), the type vss indexes and the array_* distance functions take
	auto embedding_backend_function =
	    ScalarFunction("embedding_backend", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   LogicalType::ARRAY(LogicalType::FLOAT, 384), DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, EmbeddingBackendFunction), compat_anofox::ScalarBind<DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, EmbeddingBackendBind)>());
	// SPECIAL_HANDLING so the function is still invoked when provider_config is NULL, instead of
	// DuckDB short-circuiting the whole result to NULL. This is why compute_textual_embeddings used
	// to return NULL for every row on its documented default (provider_config := NULL).
	duckdb_compat::SetNullHandling(embedding_backend_function, FunctionNullHandling::SPECIAL_HANDLING);

	duckdb_compat::SetFallible(embedding_backend_function);
	CreateScalarFunctionInfo info(embedding_backend_function);
	FunctionDescription desc;
	desc.description = "Generates a 384-dimensional text embedding (FLOAT[384] array) for the given text. "
	                   "provider (gemma-local, openai, anthropic or cohere, case-insensitive; anything else is an error) "
	                   "selects the backend. provider_config is accepted but not read yet; do not put API keys in it. NOTE: unless the extension is built with a local OpenVINO "
	                   "model or a real API backend is configured, all providers return a deterministic, "
	                   "hash-based PLACEHOLDER embedding (useful for wiring and tests, not semantic search).";
	desc.examples    = {"SELECT embedding_backend('diesel pump', 'gemma-local', '{}');",
	                    "SELECT embedding_backend('valve seat', 'openai', '');"};
	desc.categories  = {"embeddings", "textual"};
	desc.parameter_names = {"text", "provider", "provider_config"};
	desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	info.descriptions.push_back(std::move(desc));
	duckdb_compat::NameArgumentsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

static const DefaultTableMacro COMPUTE_TEXTUAL_EMBEDDINGS_MACRO = {
    DEFAULT_SCHEMA,
    "compute_textual_embeddings",
    {nullptr},
    {{"makt_table", "'sap_makt'"}, {"language", "'EN'"}, {"provider", "'gemma-local'"}, {"provider_config", "''"}, {nullptr, nullptr}},
    R"(
		WITH descriptions AS (
			-- One row per material: collapse multiple MAKT rows per (matnr, spras) (real MAKT
			-- repeats per MANDT/client) so a material is not fanned out into duplicate embeddings.
			SELECT
				material_id,
				ANY_VALUE(full_text) AS full_text
			FROM (
				SELECT
					TRIM(matnr) AS material_id,
					CONCAT_WS(' ', TRIM(maktx), TRIM(maktg)) AS full_text
				FROM query_table(makt_table)
				WHERE spras = COALESCE(language, 'EN')
					AND (TRIM(maktx) IS NOT NULL OR TRIM(maktg) IS NOT NULL)
			)
			GROUP BY material_id
		),
		embeddings AS (
			SELECT
				material_id,
				-- COALESCE(provider, 'gemma-local'): a NULL provider falls back to the default backend
				-- instead of nulling out every embedding.
				embedding_backend(full_text, COALESCE(provider, 'gemma-local'), provider_config) AS textual_embedding
			FROM descriptions
		)
		SELECT * FROM embeddings)"};

void RegisterTextualEmbeddingMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "embeddings"});
	reg.RegisterTableMacro(COMPUTE_TEXTUAL_EMBEDDINGS_MACRO,
	           {ddoc::Doc()
	                .Describe("Generates an embedding per material from its description text, returning (material_id, textual_embedding). In the released build every provider returns a deterministic hash PLACEHOLDER, not a semantic embedding. Reads descriptions from 'makt_table' in the given 'language' and embeds them with 'provider'; 'provider_config' is accepted but not read yet (do not put API keys in it).")
	                .Example("SELECT * FROM compute_textual_embeddings(makt_table := 'sap_makt', language := 'EN')")});
}

static const char *EMBED_TEXT_MACRO = R"((description := '') AS embedding_backend(description, 'gemma-local', ''))";

void RegisterEmbedTextLambdas(ExtensionLoader &loader) {
	compat_anofox::RegisterMacroInfo(loader, duckdb_compat::ScalarMacroInfo(DEFAULT_SCHEMA, "embed_text", EMBED_TEXT_MACRO),
	           {ddoc::Doc()
	                .Describe("Embeds a single piece of text with the default local backend \u2014 shorthand for embedding_backend(description, 'gemma-local', '').")
	                .Example("SELECT embed_text('centrifugal pump seal kit')")}, {"similarity", "embeddings"});
}

} // namespace anofox
} // namespace duckdb

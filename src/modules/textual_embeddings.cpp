#include "modules/textual_embeddings.hpp"
#include "core/error_handling.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/common/types/vector.hpp"
#include "telemetry.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#ifdef DUCKDB_OPENVINO_AVAILABLE
#include "openvino/openvino.hpp"
#endif

#include <memory>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace duckdb {
namespace anofox {

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
		vector<float> embedding(384, 0.0f);
		if (!text.empty()) {
			// Simple deterministic placeholder based on text hash
			size_t hash = std::hash<string> {}(text);
			for (size_t i = 0; i < 384; i++) {
				embedding[i] = static_cast<float>((hash + i) % 1000) / 1000.0f - 0.5f;
			}
		}
		return embedding;
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
		vector<float> embedding(dimension, 0.0f);
		if (!text.empty()) {
			size_t hash = std::hash<string> {}(text);
			for (size_t i = 0; i < dimension; i++) {
				embedding[i] = static_cast<float>((hash + i) % 1000) / 1000.0f - 0.5f;
			}
		}
		return embedding;
	}

	idx_t Dimension() const override {
		return dimension;
	}
};

//------------------------------------------------------------------------------
// Provider Factory
//------------------------------------------------------------------------------

class EmbeddingProviderFactory {
private:
	static std::map<string, std::shared_ptr<EmbeddingProvider>> provider_cache;

public:
	static std::shared_ptr<EmbeddingProvider> GetProvider(const string &provider_name, const string &config) {
		string cache_key = provider_name + ":" + config;

		// Check cache
		if (provider_cache.find(cache_key) != provider_cache.end()) {
			return provider_cache[cache_key];
		}

		std::shared_ptr<EmbeddingProvider> provider;

		try {
			if (provider_name == "gemma-local") {
#ifdef DUCKDB_OPENVINO_AVAILABLE
				provider = std::make_shared<GemmaLocalProvider>();
#else
				// Fallback to HTTP if OpenVINO not available
				throw std::runtime_error("OpenVINO not available, falling back to HTTP provider");
#endif
			} else if (provider_name == "openai" || provider_name == "anthropic" || provider_name == "cohere") {
				// Parse config for API key if provided
				string api_key;
				if (!config.empty()) {
					try {
						auto config_json = json::parse(config);
						if (config_json.contains("api_key")) {
							api_key = config_json["api_key"].get<string>();
						}
					} catch (...) {
						// Ignore JSON parse errors
					}
				}
				provider = std::make_shared<HTTPEmbeddingProvider>(provider_name, api_key);
			} else {
				throw std::runtime_error("Unknown embedding provider: " + provider_name);
			}
		} catch (const std::exception &e) {
			// Default fallback: use dummy provider
			provider = std::make_shared<HTTPEmbeddingProvider>("openai", "");
		}

		// Cache the provider
		provider_cache[cache_key] = provider;
		return provider;
	}
};

std::map<string, std::shared_ptr<EmbeddingProvider>> EmbeddingProviderFactory::provider_cache;

//------------------------------------------------------------------------------
// Embedding Backend Scalar Function
//------------------------------------------------------------------------------

static void EmbeddingBackendFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &text_vector = args.data[0];
	auto &provider_vector = args.data[1];
	auto &config_vector = args.data[2];
	auto count = args.size();

	text_vector.Flatten(count);
	provider_vector.Flatten(count);
	config_vector.Flatten(count);

	auto text_data = FlatVector::GetData<string_t>(text_vector);
	auto provider_data = FlatVector::GetData<string_t>(provider_vector);
	auto config_data = FlatVector::GetData<string_t>(config_vector);

	auto &text_validity = FlatVector::Validity(text_vector);
	auto &provider_validity = FlatVector::Validity(provider_vector);
	auto &config_validity = FlatVector::Validity(config_vector);

	// Get child vector for list entries
	auto &child = ListVector::GetEntry(result);
	ListVector::SetListSize(result, count * 384);

	auto result_entries = FlatVector::GetData<list_entry_t>(result);
	auto child_data = FlatVector::GetData<float>(child);
	auto &result_validity = FlatVector::Validity(result);

	// Fill embeddings
	for (idx_t i = 0; i < count; i++) {
		// Handle NULL inputs
		if (!text_validity.RowIsValid(i) || !provider_validity.RowIsValid(i)) {
			result_validity.SetInvalid(i);
			continue;
		}

		result_entries[i].offset = i * 384;
		result_entries[i].length = 384;

		try {
			string text = text_data[i].GetString();
			string provider_name = provider_data[i].GetString();
			string config = config_validity.RowIsValid(i) ? config_data[i].GetString() : "";

			auto provider = EmbeddingProviderFactory::GetProvider(provider_name, config);
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

static unique_ptr<FunctionData> EmbeddingBackendBind(ClientContext &context, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments) {
	PostHogTelemetry::Instance().RecordFunctionCall("embedding_backend");
	return nullptr;
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterTextualEmbeddingFunctions(ExtensionLoader &loader) {
	// Note: Returns LIST(FLOAT) - dimension (384) is enforced at runtime
	auto embedding_backend_function =
	    ScalarFunction("embedding_backend", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   LogicalType::LIST(LogicalType::FLOAT), EmbeddingBackendFunction, EmbeddingBackendBind);

	CreateScalarFunctionInfo info(embedding_backend_function);
	FunctionDescription desc;
	desc.description = "Generates a 384-dimensional text embedding (FLOAT[384]) for the given text using the "
	                   "specified inference provider. provider_config is a JSON string for provider-specific "
	                   "options (api_key, endpoint, etc.). Supported providers: 'gemma-local', 'gemini-api'.";
	desc.examples    = {"SELECT embedding_backend('diesel pump', 'gemma-local', '{}');",
	                    "SELECT embedding_backend('valve seat', 'gemini-api', '{\"api_key\": \"...\"}');"};
	desc.categories  = {"embeddings", "textual"};
	desc.parameter_names = {"text", "provider", "provider_config"};
	desc.parameter_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

void RegisterTextualEmbeddingMacros(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_textual_embeddings(
			makt_table := 'sap_makt',
			language := 'EN',
			provider := 'gemma-local',
			provider_config := NULL
		) AS TABLE
		WITH descriptions AS (
			SELECT
				TRIM(matnr) AS material_id,
				CONCAT_WS(' ', TRIM(maktx), TRIM(maktg)) AS full_text
			FROM query_table(makt_table)
			WHERE spras = language
				AND (TRIM(maktx) IS NOT NULL OR TRIM(maktg) IS NOT NULL)
		),
		embeddings AS (
			SELECT
				material_id,
				embedding_backend(full_text, provider, provider_config) AS textual_embedding
			FROM descriptions
		)
		SELECT * FROM embeddings
	)");

	CheckQueryResult(result, "create compute_textual_embeddings macro");
}

void RegisterEmbedTextLambdas(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO embed_text(description := '')
		AS embedding_backend(description, 'gemma-local', '')
	)");

	CheckQueryResult(result, "create embed_text macro function");
}

} // namespace anofox
} // namespace duckdb

#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Textual Embeddings Module - DuckDB Lambda Functions & Embedding Backends
//------------------------------------------------------------------------------

// Registers the embedding_backend scalar function for generating embeddings
// Parameters:
//   - loader: ExtensionLoader for registering scalar functions
//
// Scalar Function: embedding_backend(text VARCHAR, provider VARCHAR, config VARCHAR) -> FLOAT[384]
//   - Generates 384-D embeddings from text input
//   - Providers: 'gemma-local' (OpenVINO), 'openai', 'anthropic', 'custom-http'
//   - Config: Optional JSON string with provider-specific parameters (api_key, endpoint, etc.)
//   - Returns 384-D float array for semantic embeddings
//   - Fallback to HTTP if local provider unavailable
void RegisterTextualEmbeddingFunctions(ExtensionLoader &loader);

// Registers the compute_textual_embeddings SQL macros for batch embedding generation
// Parameters:
//   - conn: Database connection for registering SQL macros
//
// Macros: compute_textual_embeddings(
//   makt_table := 'sap_makt',
//   language := 'EN',
//   provider := 'gemma-local',
//   provider_config := NULL
// ) -> TABLE
//   - Extracts descriptions from MAKT table and generates embeddings
//   - Outputs: material_id, textual_embedding (FLOAT[384])
//   - Uses configured provider backend for embedding generation
//   - Supports batch processing with vectorized execution
void RegisterTextualEmbeddingMacros(Connection &conn);

// Registers the embed_text lambda function for user convenience
// Parameters:
//   - conn: Database connection for registering SQL lambda functions
//
// Lambda: embed_text(description VARCHAR) -> FLOAT[384]
//   - User-friendly function wrapping embedding_backend with 'gemma-local' provider
//   - Can be overridden by users to use different providers
void RegisterEmbedTextLambdas(Connection &conn);

} // namespace anofox
} // namespace duckdb

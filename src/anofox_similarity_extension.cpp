#include "anofox_similarity_extension.hpp"
#include "core/constants.hpp"
#include "core/error_handling.hpp"
#include "modules/jaccard_similarity.hpp"
#include "modules/similarity_search.hpp"
#include "modules/wl_kernel.hpp"
#include "modules/predecessor_inference.hpp"
#include "modules/sap_transformations.hpp"
#include "modules/dynamics365_transformations.hpp"
#include "modules/universal_schema.hpp"
#include "modules/vss_integration.hpp"
#include "modules/embedding_statistics.hpp"
#include "modules/incremental_updates.hpp"
#include "modules/textual_embeddings.hpp"
#include "modules/transactional_embeddings.hpp"
#include "modules/multimodal_fusion.hpp"
#include "modules/duckpgq_integration.hpp"
#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {

/*
 * anofox_similarity - Product Similarity Detection for Manufacturing
 * ================================================================================
 *
 * A DuckDB extension for identifying product similarities and predecessors
 * in manufacturing environments, particularly for SAP ERP systems.
 *
 * ARCHITECTURE & KEY DESIGN DECISIONS
 * ================================================================================
 *
 * 1. MODULAR DESIGN: Feature-based decomposition
 *    - Core Infrastructure: Constants, error handling
 *    - Jaccard Similarity: Component-set comparison
 *    - Similarity Search: Hybrid VSS/brute-force k-NN
 *    - WL Kernel: Structural graph similarity
 *    - Predecessor Inference: Anti-correlation temporal analysis
 *    - SAP Transformations: ERP table integration
 *    - VSS Integration: Vector similarity search infrastructure
 *
 * 2. HYBRID SIMILARITY APPROACH
 *    - Jaccard: Fast exact computation, min-hash embeddings (128-D)
 *    - WL Kernel: Graph topology beyond component membership
 *    - VSS Acceleration: HNSW indexes for ~2400x speedup
 *    - Graceful Fallback: Brute-force when embeddings unavailable
 *
 * 3. ERROR HANDLING: Explicit failure modes
 *    - REQUIRED: Core infrastructure (throw on error)
 *    - OPTIONAL: Performance features (continue on error)
 *    - BEST_EFFORT: Experimental features (silently ignore)
 *
 * 4. TEMPORAL ANALYSIS: Anti-Correlation Predecessor Detection
 *    - BOM Similarity + Consumption Anti-Correlation + Temporal Succession
 *    - Identifies predecessor materials through data-driven pattern analysis
 */

static void LoadInternal(ExtensionLoader &loader) {
	// Get database connection for SQL operations
	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

	// Core Algorithms: Jaccard similarity scalar functions
	anofox::RegisterJaccardFunctions(loader);

	// Textual Analysis: Text embedding functions
	anofox::RegisterTextualEmbeddingFunctions(loader);

	// Vector Search Infrastructure: HNSW indexes for fast similarity search
	anofox::InitializeVSSIntegration(conn);

	// ERP Integration: Universal BOM schema and conversion macros
	// Note: CreateUniversalBOMSchema should be called explicitly when needed
	// This ensures tests can control table creation and lifecycle
	anofox::RegisterBOMConversionMacros(conn);

	// Graph Analysis (Optional): DuckPGQ property graph macros for BOM traversal
	anofox::RegisterCheckDuckPGQMacro(conn);
	anofox::InitializeDuckPGQIntegration(conn);
	anofox::RegisterPropertyGraphMacros(conn);
	anofox::RegisterBOMTraversalMacros(conn);

	// Embedding Storage: Create tables and HNSW indexes for vector search
	anofox::CreateEmbeddingTables(conn);
	anofox::CreateHNSWIndexes(conn);

	// Multi-Modal Fusion: Combine embeddings from multiple sources
	anofox::RegisterMultimodalFusionFunctions(loader);

	// Similarity Search: High-level similarity search and inference macros
	anofox::RegisterSimilaritySearchMacros(conn);
	anofox::RegisterWLKernelMacros(conn);
	anofox::RegisterPredecessorInferenceMacros(conn);
	anofox::RegisterSAPTransformationMacros(conn);
	anofox::RegisterDynamics365TransformationMacros(conn);
	anofox::RegisterEmbeddingMacros(conn);
	anofox::RegisterTextualEmbeddingMacros(conn);
	anofox::RegisterEmbedTextLambdas(conn);
	anofox::RegisterFusionMacros(conn);

	// Transactional Embeddings (Optional): Time series feature integration with anofox-forecast
	// Attempt to load anofox-forecast extension before using it
	auto forecast_result = conn.Query("INSTALL anofox_forecast FROM community; LOAD anofox_forecast;");
	anofox::CheckQueryResult(forecast_result, "load anofox-forecast extension", anofox::FailureMode::OPTIONAL);

	anofox::RegisterCheckAnofoxForecastMacro(conn);
	anofox::RegisterTransactionalEmbeddingMacro(conn);

	// Feature Normalization: Embedding statistics computation for z-score normalization
	anofox::RegisterStatisticsMacros(conn);

	// Incremental Updates: Dirty material tracking and efficient refresh system
	anofox::CreateIncrementalUpdateTriggers(conn);
	anofox::RegisterIncrementalUpdateMacros(conn);
}

void AnofoxSimilarityExtension::Load(ExtensionLoader &db) {
	LoadInternal(db);
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

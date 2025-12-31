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
#include "modules/textual_embeddings.hpp"
#include "modules/transactional_embeddings.hpp"
#include "modules/multimodal_fusion.hpp"
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

	// Phase 1: Register scalar functions (Jaccard similarity)
	anofox::RegisterJaccardFunctions(loader);

	// Phase 1b: Register textual embedding functions
	anofox::RegisterTextualEmbeddingFunctions(loader);

	// Phase 2: Initialize VSS extension integration
	anofox::InitializeVSSIntegration(conn);

	// Phase 2.5: Register universal BOM schema and conversion macros
	// Note: CreateUniversalBOMSchema should be called explicitly when needed
	// This ensures tests can control table creation and lifecycle
	anofox::RegisterBOMConversionMacros(conn);

	// Phase 3: Create embedding storage and indexes
	anofox::CreateEmbeddingTables(conn);
	anofox::CreateHNSWIndexes(conn);

	// Phase 3b: Register multi-modal fusion functions
	anofox::RegisterMultimodalFusionFunctions(loader);

	// Phase 4: Register similarity search macros
	anofox::RegisterSimilaritySearchMacros(conn);
	anofox::RegisterWLKernelMacro(conn);
	anofox::RegisterPredecessorInferenceMacro(conn);
	anofox::RegisterSAPTransformationMacros(conn);
	anofox::RegisterDynamics365TransformationMacros(conn);
	anofox::RegisterEmbeddingMacros(conn);
	anofox::RegisterTextualEmbeddingMacro(conn);
	anofox::RegisterEmbedTextLambda(conn);
	anofox::RegisterFusionMacro(conn);

	// Phase 4b: Register transactional embedding macros (soft dependency on anofox-forecast)
	anofox::RegisterCheckAnofoxForecastMacro(conn);
	anofox::RegisterTransactionalEmbeddingMacro(conn);

	// Phase 5: Set up incremental update system
	anofox::CreateIncrementalUpdateTriggers(conn);
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

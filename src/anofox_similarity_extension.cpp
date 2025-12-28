#include "anofox_similarity_extension.hpp"
#include "core/constants.hpp"
#include "core/error_handling.hpp"
#include "modules/jaccard_similarity.hpp"
#include "modules/similarity_search.hpp"
#include "modules/wl_kernel.hpp"
#include "modules/predecessor_inference.hpp"
#include "modules/sap_transformations.hpp"
#include "modules/vss_integration.hpp"
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

	// Phase 2: Initialize VSS extension integration
	anofox::InitializeVSSIntegration(conn);

	// Phase 3: Create embedding storage and indexes
	anofox::CreateEmbeddingTables(conn);
	anofox::CreateHNSWIndexes(conn);

	// Phase 4: Register similarity search macros
	anofox::RegisterSimilaritySearchMacros(conn);
	anofox::RegisterWLKernelMacro(conn);
	anofox::RegisterPredecessorInferenceMacro(conn);
	anofox::RegisterSAPTransformationMacros(conn);
	anofox::RegisterEmbeddingMacros(conn);

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

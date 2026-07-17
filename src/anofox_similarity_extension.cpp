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
#include "modules/bom_utilities.hpp"
#include "modules/statistics_functions.hpp"
#include "modules/wl_kernel_cpp.hpp"
#include "modules/jaccard_cpp.hpp"
#include "modules/embedding_statistics.hpp"
#include "modules/incremental_updates.hpp"
#include "modules/textual_embeddings.hpp"
#include "modules/transactional_embeddings.hpp"
#include "modules/multimodal_fusion.hpp"
#include "modules/duckpgq_integration.hpp"
#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"

namespace duckdb {

// --- Telemetry Configuration ---
static void OnTelemetryEnabled(ClientContext &context, SetScope scope, Value &parameter) {
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetEnabled(BooleanValue::Get(parameter));
}

static void OnTelemetryKey(ClientContext &context, SetScope scope, Value &parameter) {
	auto &telemetry = PostHogTelemetry::Instance();
	telemetry.SetAPIKey(StringValue::Get(parameter));
}

static void RegisterTelemetryOptions(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());

	config.AddExtensionOption("anofox_telemetry_enabled", "Enable or disable anonymous usage telemetry",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true), OnTelemetryEnabled);

	config.AddExtensionOption("anofox_telemetry_key", "PostHog API key for telemetry", LogicalType::VARCHAR,
	                          Value(""), OnTelemetryKey);
}

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
	// Register telemetry options first
	RegisterTelemetryOptions(loader);

	// Initialize and capture extension load
	auto &telemetry = PostHogTelemetry::Instance();

	std::string version;
#ifdef EXT_VERSION_ANOFOX_SIMILARITY
	version = EXT_VERSION_ANOFOX_SIMILARITY;
#else
	version = "0.1.0";
#endif
	telemetry.SetProduct("anofox_similarity", version, "oss");
	telemetry.AssociateGroup("deployment", PostHogTelemetry::GetDistinctId());
	telemetry.CaptureExtensionLoad("anofox_similarity", version);

	// Get database connection for SQL operations
	auto &db = loader.GetDatabaseInstance();
	Connection conn(db);

	// When the primary database is opened read-only, catalog-mutating registrations
	// (CREATE MACRO / CREATE TABLE / CREATE INDEX) cannot run and — being REQUIRED — would throw
	// out of LoadInternal and abort the ENTIRE extension load, so even pure C++ scalars became
	// unusable. Detect read-only and skip only the catalog-mutating (conn-based) registrations;
	// the loader-based C++ functions below still register and work.
	const bool read_only = DBConfig::GetConfig(db).options.access_mode == AccessMode::READ_ONLY;

	// --- Loader-based C++ registrations (always safe, even read-only) ---
	anofox::RegisterJaccardFunctions(loader);
	anofox::RegisterJaccardCppFunctions(loader);
	anofox::RegisterWLKernelCppFunctions(loader, conn);
	anofox::RegisterTextualEmbeddingFunctions(loader);
	anofox::RegisterMultimodalFusionFunctions(loader);
	anofox::RegisterSimilaritySearchFunctions(loader);
	anofox::RegisterPredecessorInferenceFunctions(loader);
	anofox::RegisterEmbeddingFunctions(loader);
	anofox::RegisterTransactionalEmbeddingFunctions(loader);

	// --- Catalog-mutating (conn-based) registrations: SQL macros, infra tables, indexes ---
	// These define most of the SQL-macro API and require a writable catalog.
	if (!read_only) {
		anofox::InitializeVSSIntegration(conn);

		// ERP Integration: Universal BOM schema and conversion macros
		anofox::RegisterBOMConversionMacros(conn);

		// BOM Utilities: Helper macros for common BOM and movement filtering patterns
		anofox::RegisterBOMUtilityMacros(conn);

		// Statistics Functions: Infrastructure for efficient z-score normalization
		anofox::RegisterStatisticsFunctions(conn);

		// Graph Analysis (Optional): DuckPGQ property graph macros for BOM traversal
		anofox::RegisterCheckDuckPGQMacro(conn);
		anofox::InitializeDuckPGQIntegration(conn);
		anofox::RegisterPropertyGraphMacros(conn);
		anofox::RegisterBOMTraversalMacros(conn);

		// Embedding Storage: Create tables and HNSW indexes for vector search
		anofox::CreateEmbeddingTables(conn);
		anofox::CreateHNSWIndexes(conn);

		// Similarity-search helper macros and inference macros
		anofox::RegisterWLKernelMacros(conn);
		anofox::RegisterSAPTransformationMacros(conn);
		anofox::RegisterDynamics365TransformationMacros(conn);
		anofox::RegisterTextualEmbeddingMacros(conn);
		anofox::RegisterEmbedTextLambdas(conn);
		anofox::RegisterFusionMacros(conn);

		// Transactional Embeddings (Optional): integration with anofox-forecast
		anofox::RegisterCheckAnofoxForecastMacro(conn);

		// Feature Normalization: embedding statistics computation
		anofox::RegisterStatisticsMacros(conn);

		// Incremental Updates (no-ops today; DuckDB has no triggers / DML-in-macro)
		anofox::CreateIncrementalUpdateTriggers(conn);
		anofox::RegisterIncrementalUpdateMacros(conn);
	}
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

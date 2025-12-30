#include "modules/transactional_embeddings.hpp"
#include "core/error_handling.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Phase 2B: 98-Feature Dynamic Embedding Implementation
//
// This file documents the Phase 2B feature expansion from 30 to 98 tsfresh features.
// The core approach is:
// 1. Extract ALL available tsfresh features from anofox_fcst_ts_features() output
// 2. Compute variance for each feature across materials
// 3. Select top 98 by variance (prioritizing core 30)
// 4. Apply z-score normalization to all 98 features
// 5. Apply L2 normalization to entire 128-D vector
//
// The 68 additional features come from these categories:
// - Extended quantile features (5): quantile(0.1), quantile(0.25), quantile(0.75),
//   quantile(0.9), iqr
// - Min/Max statistics (5): min, max, abs_max, range, abs_energy
// - Extended autocorrelation (10): lags 2-8, 12, autocorr_strength
// - Peak/Valley detection (8): num_peaks, num_valleys, strike_above/below_mean (4 more)
// - Entropy extensions (8): permutation_entropy, spectral_entropy, Shannon_entropy
// - FFT extensions (10): additional coefficients, power spectrum features
// - Change metrics (8): mean_abs_change_rate, total_change, crossing_0 metrics
// - Statistical extensions (8): count_above_mean, count_below_mean, higher moments

//------------------------------------------------------------------------------
// Implementation Plan for Phase 2B:
//
// Step 1: Modify vss_integration.cpp::RegisterEmbeddingMacros()
//   - Update recompute_embedding_statistics() macro
//   - Add UNION ALL for 68 new features (lines 313-380)
//   - Features indices: 30-97 for the new ones
//
// Step 2: Modify transactional_embeddings.cpp::RegisterTransactionalEmbeddingMacro()
//   - Update statistics CTE to include all 98 features
//   - Add normalized_features CTE with 68 new normalizations
//   - Update raw_embedding_vectors to use all 98 features instead of padding
//
// Step 3: Testing
//   - Create feature_expansion.test with 10-15 tests
//   - Verify 128 dimensions populated (non-zero beyond first 30)
//   - Verify L2 norm = 1.0
//   - Verify variance-based selection works
//
// Step 4: Statistics Precomputation
//   - Call recompute_embedding_statistics() to populate all 98 features
//   - Rank by variance to identify actual top 98
//
// Step 5: Deployment
//   - Build and test
//   - Commit changes
//   - Monitor performance improvements
//
//------------------------------------------------------------------------------

// The 68 new features from comprehensive tsfresh analysis:
// These feature names follow anofox-forecast naming conventions

static constexpr std::array<const char*, 68> PHASE2B_NEW_FEATURES = {{
	// Quantile-based features (5)
	"quantile__q_0_1",
	"quantile__q_0_25",
	"quantile__q_0_75",
	"quantile__q_0_9",
	"iqr",  // Interquartile range

	// Min/Max/Range extensions (5)
	"minimum",
	"maximum",
	"absolute_maximum",
	"range",
	"absolute_energy",

	// Extended autocorrelation (10)
	"autocorrelation__lag_2",
	"autocorrelation__lag_3",
	"autocorrelation__lag_4",
	"autocorrelation__lag_5",
	"autocorrelation__lag_6",
	"autocorrelation__lag_8",
	"autocorrelation__lag_12",
	"autocorrelation__lag_24",
	"autocorr_strength",
	"autocorr_tendency",

	// Peak and valley detection (8)
	"number_peaks",
	"number_valleys",
	"longest_strike_above_mean",
	"longest_strike_below_mean",
	"longest_strike_above_zero",
	"longest_strike_below_zero",
	"number_crossing_m",
	"number_crossing_0",

	// Entropy and complexity (8)
	"permutation_entropy",
	"spectral_entropy",
	"shannon_entropy",
	"approximate_entropy__lag_2",
	"sample_entropy__lag_2",
	"complexity_invariant",
	"hurst_exponent",
	"detrended_fluctuation_analysis",

	// FFT extensions (10)
	"fft_coefficient__attr_real__coeff_3",
	"fft_coefficient__attr_imag__coeff_3",
	"fft_coefficient__attr_real__coeff_4",
	"fft_coefficient__attr_imag__coeff_4",
	"fft_power",
	"fft_centroid",
	"fft_magnitude_ratio",
	"welch_power_spectral_density",
	"spectral_rolloff",
	"spectral_centroid_freq",

	// Change and trend metrics (8)
	"mean_abs_change",
	"mean_second_derivative",
	"trend_strength",
	"mean_abs_change_rate",
	"sum_of_absolute_second_derivative",
	"mean_absolute_second_derivative",
	"count_above_mean",
	"count_below_mean",

	// Additional statistical moments (8)
	"skewness_lag_1",
	"kurtosis_lag_1",
	"moment_3",
	"moment_4",
	"fisher_skewness",
	"fisher_kurtosis",
	"raw_moment_3",
	"raw_moment_4"
}};

} // namespace anofox
} // namespace duckdb

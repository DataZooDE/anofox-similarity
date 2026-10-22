#pragma once
#include "datazoo_function_doc.hpp"

namespace compat_anofox {
// Register a parsed scalar macro with the same documentation defaults used by
// datazoo::doc::Registrar. The banner accepts DefaultMacro, while the 2.0
// compatibility parser returns a CreateMacroInfo.
inline void RegisterMacroInfo(duckdb::ExtensionLoader &loader,
                              duckdb::unique_ptr<duckdb::CreateMacroInfo> info,
                              datazoo::doc::Docs docs,
                              duckdb::vector<duckdb::string> default_categories) {
	for (auto &doc : docs) {
		if (doc.categories.empty()) {
			doc.categories = default_categories;
		}
	}
	info->descriptions = std::move(docs);
	loader.RegisterFunction(*info);
}
} // namespace compat_anofox

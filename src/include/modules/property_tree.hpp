#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Property-tree store (coverage matching, HLD §3.2 and §4.3)
//------------------------------------------------------------------------------

//! The deepest tree any function follows (levels below the root). tree_from_json,
//! tree_multiset and tree_class_multiset reject deeper trees; tree_anchors stops there;
//! validate_trees reports too_deep.
constexpr idx_t MAX_TREE_DEPTH = 1000;

//! Where the nodes of a set of trees come from and how their classes resolve.
struct TreeSource {
	string nodes_table;
	string attrs_table;     // may be empty
	string overrides_table; // reviewed vocabulary (wording, source, class_id); may be empty
	string label_table;     // classifier output (tree_id, node_id, class_id, p); may be empty
	vector<string> tree_ids; // with has_tree_ids: these trees only (none when empty); else all trees
	bool has_tree_ids = false;
	bool has_source = false;
	bool has_context = false;
};

//! Validate the table and column parameters of a TreeSource (identifiers, required columns).
void ValidateTreeSource(ClientContext &context, TreeSource &source, const string &fn);

//! SQL for the resolved nodes of a TreeSource:
//!   tree_id, node_id, parent_id, node_kind, edge_type, label, context, source, count,
//!   class_id (resolved: class_id -> reviewed vocabulary -> classifier arg-max),
//!   reviewed (true when class_id or the reviewed vocabulary gave the class)
string ResolvedNodesSQL(const TreeSource &source);

//! A SQL list literal of quoted strings, e.g. ['a', 'b'].
string QuotedList(const vector<string> &values);

//! Read a LIST(VARCHAR) named parameter.
vector<string> StringListParameter(const Value &value, const string &name);

void RegisterPropertyTreeFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb

// AAS environment -> property-tree rows (GitHub #27). See aas_tree_mapper.hpp and docs/api/17-aas.md
// for the mapping table.
#include "modules/aas/aas_tree_mapper.hpp"
#include "modules/aas/aas_values.hpp"
#include "modules/aas/semantic_key.hpp"
#include "modules/quantity.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

//------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------

bool Contains(const std::string &s, const char *part) {
	return s.find(part) != std::string::npos;
}

std::string Trim(const std::string &s) {
	return AasTrim(s);
}

std::string Lower(const std::string &s) {
	return AasLower(s);
}

// IDTA-02020 Capability Description structure (semanticId path segments, any version)
const char *CAPABILITY_CONTAINER = "/CapabilityDescription/CapabilityContainer/";
const char *PROPERTY_SET = "/CapabilityDescription/PropertySet/";
const char *PROPERTY_CONTAINER = "/CapabilityDescription/PropertyContainer/";
const char *ROLE_QUALIFIER = "/CapabilityDescription/CapabilityRoleQualifier/";
const char *PROPERTY_CONSTRAINT_CONTAINER = "/CapabilityDescription/PropertyConstraintContainer/";
const char *BASIC_CONSTRAINT = "/PropertyConstraintType/BasicConstraint/";
const char *CONSTRAINT_HAS_PROPERTY = "/CapabilityDescription/ConstraintHasProperty/";
const char *COMPOSED_OF = "/CapabilityDescription/CapabilityComposedOf/";
const char *GENERALIZED_BY = "/CapabilityDescription/CapabilityGeneralizedBy/";

//! An idShort path in one spelling for lookups: list indices as path segments (a[0] -> a.0).
std::string NormalPath(const std::string &path) {
	std::string out;
	for (char c : path) {
		if (c == '[') {
			out += '.';
		} else if (c != ']') {
			out += c;
		}
	}
	return out;
}

//! Template semanticIds name the structure of a template, not a dictionary property.
bool IsStructuralId(const std::string &id) {
	return Contains(id, "/idta/CapabilityPropertyType/") || Contains(id, "/idta/CapabilityDescription/");
}

//! The dictionary id of an element: its semanticId unless that is structural, else the first
//! supplementalSemanticId.
std::string DictionaryId(const AasElement &e) {
	if (!e.semantic_id.empty() && !IsStructuralId(e.semantic_id)) {
		return e.semantic_id;
	}
	for (auto &s : e.supplemental_ids) {
		if (!IsStructuralId(s)) {
			return s;
		}
	}
	return std::string();
}

bool Truthy(const std::string &v) {
	bool value = false;
	return ParseXsdBoolean(v, value) && value;
}

//! The longest text value compared as a category (an unkeyed or longer text is descriptive).
constexpr size_t MAX_TEXT_CATEGORY = 64;

//------------------------------------------------------------------------------
// ConceptDescriptions: the IEC 61360 dictionary entries of an environment
//------------------------------------------------------------------------------

//! Finds the dictionary entry of an id: the exact release first; any release (the id without its
//! version) only when all releases in the input agree on unit and unitId, since a property may
//! change its unit between releases (mA in #002, A in #001).
class ConceptDictionary {
public:
	explicit ConceptDictionary(const std::vector<AasConceptDescription> &cds) {
		for (auto &cd : cds) {
			if (!cd.has_data_spec) {
				continue;
			}
			auto exact_key = SemanticKey(cd.id, true).key;
			if (!exact_key.empty()) {
				exact.emplace(exact_key, &cd.data_spec);
				by_concept[SemanticKey(cd.id).key].push_back(&cd.data_spec);
			}
		}
	}

	//! nullptr when unknown; ambiguous is set when releases disagree and none matches exactly.
	const AasIec61360 *Find(const std::string &id, bool &ambiguous) const {
		auto e = exact.find(SemanticKey(id, true).key);
		if (e != exact.end()) {
			return e->second;
		}
		auto c = by_concept.find(SemanticKey(id).key);
		if (c == by_concept.end()) {
			return nullptr;
		}
		for (auto spec : c->second) {
			if (spec->unit != c->second[0]->unit || spec->unit_id != c->second[0]->unit_id) {
				ambiguous = true;
				return nullptr;
			}
		}
		return c->second[0];
	}

private:
	std::unordered_map<std::string, const AasIec61360 *> exact;
	std::unordered_map<std::string, std::vector<const AasIec61360 *>> by_concept;
};

//! The dictionary entry of a value: the element's embedded specification, completed field by field
//! from the ConceptDescription (an embedded spec with only a name does not hide the unit).
struct ResolvedSpec {
	bool found = false;
	bool ambiguous = false;
	AasIec61360 spec;
};

void Complete(AasIec61360 &into, const AasIec61360 &from) {
	if (into.preferred_name.empty()) {
		into.preferred_name = from.preferred_name;
	}
	if (into.definition.empty()) {
		into.definition = from.definition;
	}
	if (into.unit.empty() && into.unit_id.empty()) {
		into.unit = from.unit;
		into.unit_id = from.unit_id;
	}
	if (into.data_type.empty()) {
		into.data_type = from.data_type;
	}
	if (!into.has_level_type && from.has_level_type) {
		into.has_level_type = true;
		into.level_min = from.level_min;
		into.level_nom = from.level_nom;
		into.level_typ = from.level_typ;
		into.level_max = from.level_max;
	}
}

//! IDTA-02020 CapabilityRoleQualifier: required | provided | empty (NotAssigned or none).
std::string CapabilityRole(const AasElement &capability) {
	for (auto &q : capability.qualifiers) {
		auto kind = q.type;
		if (kind.empty() && Contains(q.semantic_id, ROLE_QUALIFIER)) {
			kind = Contains(q.semantic_id, "/Required/") ? "Required"
			       : Contains(q.semantic_id, "/Offered/") ? "Offered"
			                                              : "NotAssigned";
		}
		if (!Truthy(q.value)) {
			continue;
		}
		if (kind == "Required") {
			return "required";
		}
		if (kind == "Offered") {
			return "provided";
		}
	}
	return std::string();
}

//------------------------------------------------------------------------------
// The mapper
//------------------------------------------------------------------------------

class TreeBuilder {
public:
	TreeBuilder(const AasEnvironment &env, const AasMapOptions &options, AasMapping &out)
	    : env(env), options(options), units(*options.units), out(out), cds(env.concept_descriptions) {
	}

	void Build() {
		for (auto &note : env.notes) {
			Issue("", "", note.issue.c_str(), "warning", note.detail);
		}
		if (env.shells.empty() && env.submodels.empty()) {
			Issue("", "", "no_shell_or_submodel", "warning", "the input holds no shell and no submodel");
			return;
		}
		std::unordered_map<std::string, const AasSubmodel *> by_id;
		for (auto &sm : env.submodels) {
			by_id.emplace(sm.id, &sm);
		}
		std::unordered_set<std::string> used;
		for (auto &shell : env.shells) {
			auto tree_id = TreeId(shell.id, shell.id_short);
			StartTree(tree_id, shell.id_short.empty() ? shell.id : shell.id_short);
			for (auto &sm_id : shell.submodel_ids) {
				auto it = by_id.find(sm_id);
				if (it == by_id.end()) {
					Issue(tree_id, sm_id, "missing_submodel", "info",
					      "the shell references a submodel that is not in this input");
					continue;
				}
				used.insert(sm_id);
				MapSubmodel(*it->second);
			}
		}
		for (auto &sm : env.submodels) {
			if (used.count(sm.id)) {
				continue;
			}
			auto tree_id = TreeId(sm.id, sm.id_short);
			StartTree(tree_id, sm.id_short.empty() ? sm.id : sm.id_short);
			MapSubmodel(sm);
		}
	}

private:
	const AasEnvironment &env;
	const AasMapOptions &options;
	const UnitTable &units;
	AasMapping &out;
	ConceptDictionary cds;
	std::unordered_set<std::string> tree_ids;
	bool in_template = false; // a submodel of kind Template: structure without values
	std::string tree_id;
	std::string root_id;
	std::unordered_set<std::string> node_ids;

	//! An attribute by position: out.nodes[node].attrs[attr].
	struct AttrRef {
		idx_t node;
		idx_t attr;
	};
	//! Attributes of the current submodel by element path (NormalPath), for ConstraintHasProperty.
	std::unordered_map<std::string, std::vector<AttrRef>> attrs_by_path;
	//! An IDTA-02020 BasicConstraint waiting for the whole submodel to be mapped.
	struct PendingConstraint {
		std::string constraint_path;
		std::string expression;
		AasReference target;
	};
	std::vector<PendingConstraint> constraints;
	//! IDTA-02020 capability relations, resolved once the submodel is mapped.
	struct PendingRelation {
		bool composed; // ComposedOf (else GeneralizedBy)
		std::string path;
		AasReference first;
		AasReference second;
	};
	std::vector<PendingRelation> relations;
	//! CapabilityContainer nodes by the paths a reference may use: the container's and its Capability's.
	std::unordered_map<std::string, idx_t> capability_nodes;
	std::unordered_set<std::string> class_edges; // dedupe "child\x1fparent"

	//! The tree id: the shell (or submodel) id, or the given tree_id for the first tree of the input;
	//! further trees get the given id plus their idShort (their id when idShorts collide).
	std::string TreeId(const std::string &id, const std::string &id_short) {
		std::string result;
		if (options.tree_id.empty()) {
			result = id.empty() ? id_short : id;
		} else if (tree_ids.empty()) {
			result = options.tree_id;
		} else {
			result = options.tree_id + "/" + (id_short.empty() ? id : id_short);
			if (tree_ids.count(result)) {
				result = options.tree_id + "/" + id;
			}
			Issue(result, "", "tree_id_suffixed", "info",
			      "the input holds several trees; '" + options.tree_id + "' is the first, this one is '" + result + "'");
		}
		auto base = result;
		for (idx_t n = 2; tree_ids.count(result); n++) {
			result = base + "~" + std::to_string(n);
		}
		tree_ids.insert(result);
		return result;
	}

	void StartTree(const std::string &id, const std::string &label) {
		tree_id = id;
		root_id = id;
		node_ids.clear();
		TreeNodeRow root;
		root.tree_id = id;
		root.node_id = id;
		root.node_kind = "container";
		root.label = label;
		Add(std::move(root));
	}

	//! Adds a node with a unique node id and returns its index. A clash (two siblings with one
	//! idShort) is renamed X~2 so the rows stay unique, and reported.
	idx_t Add(TreeNodeRow node) {
		auto base = node.node_id;
		for (idx_t n = 2; node_ids.count(node.node_id); n++) {
			node.node_id = base + "~" + std::to_string(n);
		}
		if (node.node_id != base) {
			Issue(tree_id, base, "duplicate_id_short", "warning", "a second element with this path, kept as " + node.node_id);
		}
		node_ids.insert(node.node_id);
		out.nodes.push_back(std::move(node));
		return out.nodes.size() - 1;
	}

	void Issue(const std::string &tree, const std::string &path, const char *issue, const char *severity,
	           const std::string &detail) {
		out.issues.push_back({tree, path, issue, severity, detail});
	}

	//! The dictionary entry of a value element (and of the element that names it, owner): the
	//! embedded specification first, completed from the ConceptDescription of a semanticId or a
	//! supplementalSemanticId.
	ResolvedSpec Spec(const AasElement &e, const AasElement *owner) const {
		ResolvedSpec r;
		for (auto el : {&e, owner}) {
			if (el && el->has_data_spec) {
				if (!r.found) {
					r.spec = el->data_spec;
				} else {
					Complete(r.spec, el->data_spec);
				}
				r.found = true;
			}
		}
		for (auto el : {&e, owner}) {
			if (!el) {
				continue;
			}
			std::vector<std::string> ids {el->semantic_id};
			ids.insert(ids.end(), el->supplemental_ids.begin(), el->supplemental_ids.end());
			for (auto &id : ids) {
				if (id.empty()) {
					continue;
				}
				auto cd = cds.Find(id, r.ambiguous);
				if (cd) {
					if (!r.found) {
						r.spec = *cd;
					} else {
						Complete(r.spec, *cd);
					}
					r.found = true;
					return r;
				}
			}
		}
		return r;
	}

	//! A quantity written as text ("24 V DC", "max. 38 dB(A)"). A bare number in a string is an
	//! identifier, a version or a date part (ProductClassId 0112/2///62683#ACC501, "15.0",
	//! "2025-03-15"), not a count: text needs a written unit.
	bool TextQuantity(const std::string &text, Quantity &q) const {
		q = ParseQuantity(text, vector<string>(options.langs.begin(), options.langs.end()), units);
		return q.has_value && !q.dimension.empty() && !q.source_unit.empty();
	}

	//! A text value compared as a category: a code such as IP65 (parse_quantity), or a short text of
	//! a property with a dictionary id ("EtherCAT").
	bool TextCategory(const AasElement &e, const AasElement *owner, std::string &category) const {
		Quantity q;
		auto text = Trim(e.value);
		q = ParseQuantity(text, vector<string>(options.langs.begin(), options.langs.end()), units);
		if (!q.category.empty() && !q.has_value) {
			category = q.category;
			return true;
		}
		bool keyed = !DictionaryId(e).empty() || (owner && !DictionaryId(*owner).empty());
		if (keyed && !text.empty() && text.size() <= MAX_TEXT_CATEGORY && text.find('\n') == std::string::npos) {
			category = text;
			return true;
		}
		return false;
	}

	//! A Property or Range that yields an attribute: a number (also an absent or unparseable one,
	//! so a requirement shows up as a gap), a range, an enumeration value, a boolean, a quantity
	//! written as text or a coded text. Descriptive text (a manufacturer name) is not one, and
	//! template submodels contribute only stated values.
	bool IsValueLeaf(const AasElement &e, const AasElement *owner = nullptr) const {
		bool numeric = IsNumericXsdType(e.value_type);
		if (e.model_type == "Range") {
			bool stated = (e.has_min && !Trim(e.min).empty()) || (e.has_max && !Trim(e.max).empty());
			return stated || (numeric && !in_template);
		}
		if (e.model_type != "Property") {
			return false;
		}
		if (!e.value_id.empty()) {
			return true;
		}
		bool stated = e.has_value && !Trim(e.value).empty();
		if (!stated) {
			return numeric && !in_template;
		}
		if (Lower(e.value_type) == "xs:boolean" || numeric) {
			return true;
		}
		Quantity q;
		std::string category;
		return TextQuantity(e.value, q) || TextCategory(e, owner, category);
	}

	//! A SubmodelElementList of values: one multi-valued property (e.g. the housing materials).
	bool IsValueList(const AasElement &e) const {
		if (e.model_type != "SubmodelElementList" || e.children.empty()) {
			return false;
		}
		for (auto &c : e.children) {
			if (!IsValueLeaf(c, &e)) {
				return false;
			}
		}
		return true;
	}

	bool HasDirectValues(const std::vector<AasElement> &children) const {
		for (auto &c : children) {
			if (IsValueLeaf(c) || IsValueList(c)) {
				return true;
			}
		}
		return false;
	}

	static std::string ChildPath(const std::string &parent, const AasElement &e, idx_t index, bool in_list) {
		if (in_list || e.id_short.empty()) {
			return parent + "[" + std::to_string(index) + "]";
		}
		return parent.empty() ? e.id_short : parent + "." + e.id_short;
	}

	static std::string Label(const AasElement &e) {
		if (!e.display_name.empty()) {
			return e.display_name;
		}
		return HumaniseIdShort(e.id_short);
	}

	void MapSubmodel(const AasSubmodel &sm) {
		TreeNodeRow node;
		node.tree_id = tree_id;
		node.node_id = sm.id_short.empty() ? sm.id : sm.id_short;
		node.parent_id = root_id;
		node.label = HumaniseIdShort(sm.id_short);
		node.context = sm.id_short;
		bool item = HasDirectValues(sm.elements);
		node.node_kind = item ? "item" : "container";
		if (item) {
			node.class_id = SemanticKey(sm.semantic_id).key;
		}
		auto path = node.node_id;
		in_template = sm.kind == "Template";
		auto index = Add(std::move(node));
		attrs_by_path.clear();
		constraints.clear();
		relations.clear();
		capability_nodes.clear();
		MapChildren(sm.elements, index, path, false, "");
		ApplyConstraints();
		ApplyRelations();
	}

	//------------------------------------------------------------------------------
	// IDTA-02020 capability relations: CapabilityComposedOf (first is composed of second) makes the
	// sub-capability a child of the composite; CapabilityGeneralizedBy (second is more general)
	// gives a class hierarchy edge.
	//------------------------------------------------------------------------------

	void CollectRelations(const AasElement &e, const std::string &path) {
		for (idx_t i = 0; i < e.children.size(); i++) {
			auto &c = e.children[i];
			auto child_path = ChildPath(path, c, i, e.model_type == "SubmodelElementList");
			if (Contains(c.semantic_id, COMPOSED_OF) || Contains(c.semantic_id, GENERALIZED_BY)) {
				relations.push_back({Contains(c.semantic_id, COMPOSED_OF), child_path, c.first, c.second});
			} else if (AasIsContainer(c.model_type) && !Contains(c.semantic_id, CAPABILITY_CONTAINER)) {
				CollectRelations(c, child_path);
			}
		}
	}

	const TreeNodeRow *Node(const std::string &node_id) const {
		for (auto &n : out.nodes) {
			if (n.tree_id == tree_id && n.node_id == node_id) {
				return &n;
			}
		}
		return nullptr;
	}

	//! True when candidate is node or one of its ancestors.
	bool IsAncestorOrSelf(const std::string &candidate, const std::string &node_id) const {
		std::string cur = node_id;
		for (idx_t steps = 0; !cur.empty() && steps <= out.nodes.size(); steps++) {
			if (cur == candidate) {
				return true;
			}
			auto n = Node(cur);
			cur = n ? n->parent_id : std::string();
		}
		return false;
	}

	void ApplyRelations() {
		std::unordered_set<idx_t> recomposed;
		for (auto &r : relations) {
			auto first = capability_nodes.find(ReferencePath(r.first));
			if (first == capability_nodes.end()) {
				Issue(tree_id, r.path, "unresolved_relation", "warning",
				      "the relation's first element " + r.first.Last() + " is not a mapped capability");
				continue;
			}
			auto second = capability_nodes.find(ReferencePath(r.second));
			if (!r.composed) {
				auto &child = out.nodes[first->second].class_id;
				auto parent = second != capability_nodes.end() ? out.nodes[second->second].class_id
				                                               : SemanticKey(r.second.Last()).key; // external ontology
				if (!child.empty() && !parent.empty() && child != parent && class_edges.insert(child + "\x1f" + parent).second) {
					out.class_hierarchy.push_back({child, parent});
				}
				continue;
			}
			if (second == capability_nodes.end()) {
				Issue(tree_id, r.path, "unresolved_relation", "warning",
				      "ComposedOf names " + r.second.Last() + ", which is not a mapped capability");
				continue;
			}
			auto &composite = out.nodes[first->second];
			auto &part = out.nodes[second->second];
			if (IsAncestorOrSelf(part.node_id, composite.node_id)) {
				Issue(tree_id, r.path, "composition_cycle", "warning",
				      part.node_id + " cannot be part of " + composite.node_id + ": it contains it");
			} else if (!recomposed.insert(second->second).second) {
				Issue(tree_id, r.path, "multiple_composition", "info",
				      part.node_id + " is already part of " + part.parent_id + "; kept there");
			} else {
				part.parent_id = composite.node_id;
				part.context = composite.node_id;
				part.edge_type = "AND";
			}
		}
	}

	//------------------------------------------------------------------------------
	// IDTA-02020 property constraints: a BasicConstraint ("<= 1 ms", ">= 7.6 A") and the
	// ConstraintHasProperty relations that name the constrained properties.
	//------------------------------------------------------------------------------

	void CollectConstraints(const AasElement &e, const std::string &path) {
		for (idx_t i = 0; i < e.children.size(); i++) {
			auto &c = e.children[i];
			auto child_path = ChildPath(path, c, i, e.model_type == "SubmodelElementList");
			if (Contains(c.semantic_id, PROPERTY_CONSTRAINT_CONTAINER)) {
				CollectConstraintContainer(c, child_path);
			} else if (AasIsContainer(c.model_type)) {
				CollectConstraints(c, child_path);
			}
		}
	}

	void CollectConstraintContainer(const AasElement &container, const std::string &path) {
		std::string expression;
		std::vector<AasReference> targets;
		std::vector<const AasElement *> stack {&container};
		while (!stack.empty()) {
			auto e = stack.back();
			stack.pop_back();
			for (auto &c : e->children) {
				if (c.model_type == "Property" && Contains(c.semantic_id, BASIC_CONSTRAINT)) {
					expression = Trim(c.value);
				} else if (Contains(c.semantic_id, CONSTRAINT_HAS_PROPERTY)) {
					targets.push_back(c.second);
				} else if (AasIsContainer(c.model_type)) {
					stack.push_back(&c);
				}
			}
		}
		if (expression.empty()) {
			return; // OCL, operation or custom constraints: not evaluated
		}
		for (auto &t : targets) {
			constraints.push_back({path, expression, t});
		}
	}

	//! The element path a ModelReference names: the submodel's idShort, then the idShorts (or list
	//! indices) of its keys.
	std::string ReferencePath(const AasReference &r) const {
		std::string path;
		for (auto &k : r.keys) {
			if (k.first == "Submodel") {
				for (auto &sm : env.submodels) {
					if (sm.id == k.second) {
						path = sm.id_short;
					}
				}
			} else if (k.first != "AssetAdministrationShell") {
				path += (path.empty() ? "" : ".") + k.second;
			}
		}
		return NormalPath(path);
	}

	void ApplyConstraints() {
		for (auto &c : constraints) {
			auto it = attrs_by_path.find(ReferencePath(c.target));
			if (it == attrs_by_path.end()) {
				Issue(tree_id, c.constraint_path, "unresolved_constraint", "warning",
				      "ConstraintHasProperty names " + c.target.Last() + ", which is not a mapped property");
				continue;
			}
			auto q = ParseQuantity(c.expression, vector<string>(options.langs.begin(), options.langs.end()), units);
			if (q.bound.empty()) {
				Issue(tree_id, c.constraint_path, "unparsed_constraint", "info",
				      "BasicConstraint '" + c.expression + "' states no bound");
				continue;
			}
			for (auto &ref : it->second) {
				auto &a = out.nodes[ref.node].attrs[ref.attr];
				a.bound = q.bound;
				a.bound_source = "constraint";
			}
		}
	}

	void MapChildren(const std::vector<AasElement> &children, idx_t parent, const std::string &path, bool in_list,
	                 const std::string &role) {
		for (idx_t i = 0; i < children.size(); i++) {
			auto &c = children[i];
			auto child_path = ChildPath(path, c, i, in_list);
			if (IsValueLeaf(c)) {
				AddAttribute(parent, c, nullptr, child_path, AttrId(c, child_path), Label(c), role);
			} else if (IsValueList(c)) {
				for (idx_t k = 0; k < c.children.size(); k++) {
					AddAttribute(parent, c.children[k], &c, ChildPath(child_path, c.children[k], k, true),
					             AttrId(c, child_path) + "[" + std::to_string(k) + "]", Label(c), role);
				}
			} else if (AasIsContainer(c.model_type) && Contains(c.semantic_id, CAPABILITY_CONTAINER)) {
				MapCapabilityContainer(c, parent, child_path);
			} else if (AasIsContainer(c.model_type)) {
				MapCollection(c, parent, child_path, role);
			} else if (c.model_type == "Capability") {
				MapCapability(c, parent, child_path);
			}
		}
	}

	void MapCollection(const AasElement &e, idx_t parent, const std::string &path, const std::string &role) {
		TreeNodeRow node;
		node.tree_id = tree_id;
		node.node_id = path;
		node.parent_id = out.nodes[parent].node_id;
		node.label = Label(e);
		node.context = out.nodes[parent].node_id;
		bool item = HasDirectValues(e.children);
		node.node_kind = item ? "item" : "container";
		if (item) {
			auto id = DictionaryId(e);
			node.class_id = id.empty() ? std::string() : SemanticKey(id).key;
		}
		auto index = Add(std::move(node));
		MapChildren(e.children, index, path, e.model_type == "SubmodelElementList", role);
	}

	//! A Capability outside a CapabilityContainer: an item without attributes.
	void MapCapability(const AasElement &e, idx_t parent, const std::string &path) {
		TreeNodeRow node;
		node.tree_id = tree_id;
		node.node_id = path;
		node.parent_id = out.nodes[parent].node_id;
		node.node_kind = "item";
		node.label = Label(e);
		node.context = out.nodes[parent].node_id;
		node.class_id = CapabilityClass(e);
		Add(std::move(node));
	}

	static std::string CapabilityClass(const AasElement &capability) {
		auto id = DictionaryId(capability);
		return id.empty() ? capability.id_short : SemanticKey(id).key;
	}

	//! IDTA-02020 CapabilityContainer: the Capability gives class, label and role; the property
	//! sets give the attributes.
	void MapCapabilityContainer(const AasElement &e, idx_t parent, const std::string &path) {
		const AasElement *capability = nullptr;
		for (auto &c : e.children) {
			if (c.model_type == "Capability") {
				capability = &c;
				break;
			}
		}
		TreeNodeRow node;
		node.tree_id = tree_id;
		node.node_id = path;
		node.parent_id = out.nodes[parent].node_id;
		node.node_kind = "item";
		node.context = out.nodes[parent].node_id;
		std::string role;
		if (capability) {
			node.label = Label(*capability);
			node.class_id = CapabilityClass(*capability);
			role = CapabilityRole(*capability);
		} else {
			node.label = Label(e);
			Issue(tree_id, path, "capability_missing", "warning", "a CapabilityContainer without a Capability element");
		}
		auto index = Add(std::move(node));
		capability_nodes[NormalPath(path)] = index;
		if (capability) {
			capability_nodes[NormalPath(path + "." + capability->id_short)] = index;
		}
		CollectRelations(e, path);
		for (auto &c : e.children) {
			if (Contains(c.semantic_id, PROPERTY_SET)) {
				MapPropertySet(c, index, ChildPath(path, c, 0, false), role);
			}
		}
		CollectConstraints(e, path);
	}

	void MapPropertySet(const AasElement &set, idx_t item, const std::string &path, const std::string &role) {
		for (idx_t i = 0; i < set.children.size(); i++) {
			auto &container = set.children[i];
			auto container_path = ChildPath(path, container, i, false);
			if (!Contains(container.semantic_id, PROPERTY_CONTAINER)) {
				continue;
			}
			for (idx_t j = 0; j < container.children.size(); j++) {
				auto &p = container.children[j];
				MapCapabilityProperty(p, container, Label(container), item, ChildPath(container_path, p, j, false),
				                      AttrId(container, container_path), role);
			}
		}
	}

	//! PropertyRange / PropertyProperty, or a PropertySubmodelList of them. The PropertyContainer
	//! names the property (label, attr_id); the dictionary id comes from the element, else from the
	//! list that holds it, else from the container (owner).
	void MapCapabilityProperty(const AasElement &p, const AasElement &owner, const std::string &label, idx_t item,
	                           const std::string &path, const std::string &attr_id, const std::string &role) {
		if (p.model_type == "SubmodelElementList") {
			auto &list_owner = DictionaryId(p).empty() ? owner : p;
			for (idx_t k = 0; k < p.children.size(); k++) {
				MapCapabilityProperty(p.children[k], list_owner, label, item, ChildPath(path, p.children[k], k, true),
				                      attr_id + "[" + std::to_string(k) + "]", role);
			}
			return;
		}
		if (IsValueLeaf(p, &owner)) {
			AddAttribute(item, p, &owner, path, attr_id, label, role);
		}
	}

	static std::string AttrId(const AasElement &e, const std::string &path) {
		if (!e.id_short.empty()) {
			return e.id_short;
		}
		auto bracket = path.rfind('[');
		return bracket == std::string::npos ? path : path.substr(bracket);
	}

	//------------------------------------------------------------------------------
	// Values
	//------------------------------------------------------------------------------

	//! owner: the element that names a value when e itself does not (a value list, an IDTA-02020
	//! PropertyContainer); its dictionary entry and id are the fallback.
	void AddAttribute(idx_t node_index, const AasElement &e, const AasElement *owner, const std::string &path,
	                  const std::string &attr_id, const std::string &label, const std::string &role) {
		auto &node = out.nodes[node_index];
		TreeAttrRow a;
		a.attr_id = attr_id;
		std::unordered_set<std::string> taken;
		for (auto &x : node.attrs) {
			taken.insert(x.attr_id);
		}
		auto base = a.attr_id;
		for (idx_t n = 2; taken.count(a.attr_id); n++) {
			a.attr_id = base + "~" + std::to_string(n);
		}
		if (a.attr_id != base) {
			Issue(tree_id, path, "duplicate_id_short", "warning", "a second value named " + base + ", kept as " + a.attr_id);
		}
		auto resolved = Spec(e, owner);
		if (resolved.ambiguous && !resolved.found) {
			Issue(tree_id, path, "ambiguous_concept_description", "warning",
			      "no ConceptDescription for this release, and the other releases disagree on the unit");
		}
		auto spec = resolved.found ? &resolved.spec : nullptr;
		a.name = spec && !spec->preferred_name.empty() ? spec->preferred_name : label;
		a.attr_key = DictionaryId(e);
		if (a.attr_key.empty() && owner) {
			a.attr_key = DictionaryId(*owner);
		}
		a.role = role;
		if (e.model_type == "Range") {
			MapRange(e, spec, resolved.ambiguous, path, a);
		} else {
			MapProperty(e, owner, spec, resolved.ambiguous, path, a);
		}
		node.attrs.push_back(std::move(a));
		AttrRef ref {node_index, node.attrs.size() - 1};
		attrs_by_path[NormalPath(path)].push_back(ref);
		if (owner) {
			// a constraint may name the PropertyContainer (or the list) instead of the value element
			auto dot = NormalPath(path).rfind('.');
			if (dot != std::string::npos) {
				attrs_by_path[NormalPath(path).substr(0, dot)].push_back(ref);
			}
		}
	}

	//! The unit of a numeric value: unitId first, then the unit text. Returns false when unknown.
	bool ResolveUnit(const AasElement &e, const AasIec61360 *spec, bool ambiguous, const std::string &path,
	                 const UnitDef *&def, std::string &written) {
		def = nullptr;
		written.clear();
		if (!spec) {
			if (ambiguous) {
				return false; // reported as ambiguous_concept_description
			}
			if (!e.semantic_id.empty() || !e.supplemental_ids.empty()) {
				Issue(tree_id, path, "missing_concept_description", "warning",
				      "no ConceptDescription for " + (e.semantic_id.empty() ? e.supplemental_ids[0] : e.semantic_id) +
				          ": the unit is unknown");
			} else {
				Issue(tree_id, path, "no_unit_information", "warning",
				      "a numeric value without semanticId or data specification: the unit is unknown");
			}
			return false;
		}
		if (!spec->unit_id.empty()) {
			def = units.FindById(spec->unit_id);
			if (!def) {
				Issue(tree_id, path, "unknown_unit_id", "warning",
				      "unitId " + spec->unit_id + " is not in the unit table" +
				          (spec->unit.empty() ? std::string() : "; using the unit text '" + spec->unit + "'"));
			}
		}
		if (!def && !spec->unit.empty()) {
			written = Trim(spec->unit);
			def = units.FindSymbolOrId(written);
		}
		if (spec->unit.empty() && spec->unit_id.empty()) {
			Issue(tree_id, path, "assumed_dimensionless", "info",
			      "the dictionary entry states no unit: the value is read as a count");
			return true;
		}
		return def != nullptr;
	}

	//! Reports a number that is not a valid XSD literal; true when a value was read.
	bool CheckNumber(const XsdNumber &n, const std::string &text, const std::string &value_type,
	                 const std::string &path) {
		switch (n.status) {
		case XsdNumberStatus::OK:
			return true;
		case XsdNumberStatus::DECIMAL_COMMA:
			Issue(tree_id, path, "decimal_comma", "info", "'" + text + "' read as " + n.read_as);
			return true;
		case XsdNumberStatus::AMBIGUOUS:
			Issue(tree_id, path, "bad_value", "warning",
			      "'" + text + "' is ambiguous: ',' may be a thousands or a decimal separator");
			return false;
		case XsdNumberStatus::NOT_INTEGER:
			Issue(tree_id, path, "bad_value", "warning", "'" + text + "' is not an integer (" + value_type + ")");
			return false;
		default:
			Issue(tree_id, path, "bad_value", "warning", "'" + text + "' is not a valid " + value_type + " value");
			return false;
		}
	}

	//! Converts lo/hi into the base unit of the resolved unit; without a unit the value is a count.
	void ApplyUnit(const UnitDef *def, bool known, const std::string &written, TreeAttrRow &a) {
		if (def) {
			a.dimension = def->dimension;
			if (std::isnan(def->factor)) {
				a.unit = def->symbol;
			} else {
				a.lo = a.lo * def->factor + def->offset;
				a.hi = a.hi * def->factor + def->offset;
				a.unit = units.BaseUnit(def->dimension);
			}
		} else if (known) {
			a.dimension = "count"; // a dictionary entry without a unit: a plain number
			a.unit = "1";
		} else {
			a.unit = written; // unknown: kept as written, dimension NULL (validate_trees: unknown_unit)
		}
	}

	void MapRange(const AasElement &e, const AasIec61360 *spec, bool ambiguous, const std::string &path,
	              TreeAttrRow &a) {
		bool has_lo = e.has_min && !Trim(e.min).empty();
		bool has_hi = e.has_max && !Trim(e.max).empty();
		a.value_text = (has_lo ? Trim(e.min) : std::string()) + ".." + (has_hi ? Trim(e.max) : std::string());
		if (!has_lo && !has_hi) {
			a.value_text.clear();
			Issue(tree_id, path, "empty_value", "warning", "a Range without min and max: nothing to compare");
			return;
		}
		bool integer = IsIntegerXsdType(e.value_type);
		XsdNumber lo, hi;
		if (has_lo) {
			lo = ParseXsdNumber(e.min, integer);
		}
		if (has_hi) {
			hi = ParseXsdNumber(e.max, integer);
		}
		if ((has_lo && !CheckNumber(lo, Trim(e.min), e.value_type, path)) ||
		    (has_hi && !CheckNumber(hi, Trim(e.max), e.value_type, path))) {
			return;
		}
		const UnitDef *def;
		std::string written;
		bool known = ResolveUnit(e, spec, ambiguous, path, def, written);
		a.has_value = true;
		a.lo = has_lo ? lo.value : hi.value;
		a.hi = has_hi ? hi.value : lo.value;
		ApplyUnit(def, known, written, a);
		a.bound = has_lo && has_hi ? "within" : has_lo ? "at_least" : "at_most";
		a.bound_source = "range";
	}

	void MapProperty(const AasElement &e, const AasElement *owner, const AasIec61360 *spec, bool ambiguous,
	                 const std::string &path, TreeAttrRow &a) {
		a.value_text = Trim(e.value);
		if (!e.value_id.empty()) {
			a.category = SemanticKey(e.value_id).key; // an enumeration value
			return;
		}
		auto type = Lower(e.value_type);
		if (type == "xs:boolean") {
			bool value = false;
			if (ParseXsdBoolean(e.value, value)) {
				a.category = value ? "true" : "false";
			} else {
				Issue(tree_id, path, "bad_value", "warning", "'" + a.value_text + "' is not an xs:boolean (true, false, 1, 0)");
			}
			return;
		}
		if (IsNumericXsdType(type)) {
			if (a.value_text.empty()) {
				Issue(tree_id, path, "empty_value", "warning", "a numeric property without a value: nothing to compare");
				return;
			}
			auto n = ParseXsdNumber(e.value, IsIntegerXsdType(type));
			if (!CheckNumber(n, a.value_text, e.value_type, path)) {
				return;
			}
			const UnitDef *def;
			std::string written;
			bool known = ResolveUnit(e, spec, ambiguous, path, def, written);
			a.has_value = true;
			a.lo = a.hi = n.value;
			ApplyUnit(def, known, written, a);
			ApplyLevelType(spec, a);
			return;
		}
		// text: a quantity written out ("24 V DC", "max. 38 dB(A)"), else a code (IsValueLeaf
		// admitted nothing else)
		Quantity q;
		if (TextQuantity(a.value_text, q)) {
			a.has_value = true;
			a.lo = q.lo;
			a.hi = q.hi;
			a.dimension = q.dimension;
			a.unit = q.unit;
			if (!q.bound.empty()) {
				a.bound = q.bound;
				a.bound_source = "text";
			}
			return;
		}
		TextCategory(e, owner, a.category);
	}

	//! IEC 61360 levelType of a single value: exactly min -> at_least, max -> at_most,
	//! nom -> equals. typ or several levels state no direction.
	static void ApplyLevelType(const AasIec61360 *spec, TreeAttrRow &a) {
		if (!spec || !spec->has_level_type || !a.bound.empty()) {
			return;
		}
		int n = int(spec->level_min) + int(spec->level_max) + int(spec->level_nom) + int(spec->level_typ);
		if (n != 1 || spec->level_typ) {
			return;
		}
		a.bound = spec->level_min ? "at_least" : spec->level_max ? "at_most" : "equals";
		a.bound_source = "level_type";
	}
};

} // namespace

std::string HumaniseIdShort(const std::string &id_short) {
	std::vector<std::string> words;
	std::string cur;
	auto flush = [&]() {
		if (!cur.empty()) {
			words.push_back(cur);
			cur.clear();
		}
	};
	for (size_t i = 0; i < id_short.size(); i++) {
		char c = id_short[i];
		if (c == '_' || c == '-' || c == ' ') {
			flush();
			continue;
		}
		bool upper = std::isupper(static_cast<unsigned char>(c)) != 0;
		if (upper && !cur.empty()) {
			char prev = cur.back();
			bool prev_lower = std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev));
			bool next_lower = i + 1 < id_short.size() && std::islower(static_cast<unsigned char>(id_short[i + 1]));
			bool prev_upper = std::isupper(static_cast<unsigned char>(prev)) != 0;
			if (prev_lower || (prev_upper && next_lower)) {
				flush();
			}
		}
		cur += c;
	}
	flush();
	std::string out;
	for (size_t w = 0; w < words.size(); w++) {
		auto word = words[w];
		bool acronym = word.size() > 1 && std::isupper(static_cast<unsigned char>(word[1]));
		if (w > 0 && !acronym) {
			word[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(word[0])));
		} else if (w == 0) {
			word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
		}
		out += (w ? " " : "") + word;
	}
	return out;
}

AasMapping MapAasToTrees(const AasEnvironment &env, const AasMapOptions &options) {
	AasMapping out;
	TreeBuilder(env, options, out).Build();
	return out;
}

} // namespace anofox
} // namespace duckdb

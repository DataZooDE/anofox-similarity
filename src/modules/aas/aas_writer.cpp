// Property trees and coverage results -> AAS v3 Part 1 JSON (GitHub #34). See aas_writer.hpp.
#include "modules/aas/aas_writer.hpp"
#include "modules/aas/aas_values.hpp"

#include "duckdb/common/exception.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

using Json = nlohmann::ordered_json;

const char *CD = "https://admin-shell.io/idta/CapabilityDescription/";
const char *CPT = "https://admin-shell.io/idta/CapabilityPropertyType/";
const char *IEC61360 = "https://admin-shell.io/DataSpecificationTemplates/DataSpecificationIec61360/3/0";
const char *ANOFOX = ANOFOX_AAS_NAMESPACE;
constexpr size_t AAS_ID_SHORT_MAX = 128;

Json Ref(const std::string &value) {
	return Json {{"type", "ExternalReference"}, {"keys", Json::array({{{"type", "GlobalReference"}, {"value", value}}})}};
}

Json LangText(const std::string &text, size_t max) {
	return Json::array({{{"language", "en"}, {"text", text.substr(0, max)}}});
}

//! The shortest decimal that reads back as the same double; XSD spellings for the non-finite.
std::string Number(double v) {
	if (std::isnan(v)) {
		return "NaN";
	}
	if (std::isinf(v)) {
		return v > 0 ? "INF" : "-INF";
	}
	char buf[40];
	for (int precision = 15; precision <= 17; precision++) {
		std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
		if (std::strtod(buf, nullptr) == v) {
			break;
		}
	}
	return buf;
}

//! An identifier-like category (written as valueId): an IRDI (four digits, '-' or '/', then '#') or a
//! URI. Free text with a '#' ("C#") is not one.
bool IsIdentifier(const std::string &s) {
	bool irdi = s.size() > 6 && std::isdigit(static_cast<unsigned char>(s[0])) &&
	            std::isdigit(static_cast<unsigned char>(s[3])) && (s[4] == '-' || s[4] == '/') &&
	            s.find('#') != std::string::npos;
	return irdi || s.find("://") != std::string::npos || s.rfind("urn:", 0) == 0;
}

//! The numeric index of a list member "X[12]" (or of a node id ending in "[12]"); -1 otherwise.
long ListIndex(const std::string &id) {
	auto open = id.rfind('[');
	if (open == std::string::npos || id.back() != ']') {
		return -1;
	}
	return std::strtol(id.c_str() + open + 1, nullptr, 10);
}

Json Qualifier(const std::string &type, const std::string &value_type, const std::string &value) {
	return Json {{"kind", "ConceptQualifier"}, {"type", type}, {"valueType", value_type}, {"value", value}};
}

//! The IEC 61360 data specification of a value: its name and unit (the unit as stored, i.e.
//! the base unit of its dimension; none for counts). level is the levelType that carries a bound a
//! Property cannot state otherwise: "nom" (equals), "min" / "max" (an ordinal at_least / at_most).
Json Iec61360(const TreeAttrRow &a, const std::string &level) {
	Json content {{"modelType", "DataSpecificationIec61360"},
	              {"preferredName", LangText(a.name.empty() ? a.attr_id : a.name, 255)}};
	if (!a.unit.empty() && a.unit != "1") {
		content["unit"] = a.unit;
	}
	if (!level.empty()) {
		content["levelType"] =
		    Json {{"min", level == "min"}, {"nom", level == "nom"}, {"typ", false}, {"max", level == "max"}};
	}
	return Json::array({{{"dataSpecification", Ref(IEC61360)}, {"dataSpecificationContent", content}}});
}

//! A number whose unit is unknown carries no dictionary entry: an entry without a unit would read
//! back as a count (assumed_dimensionless), while no entry reads back as unknown again.
bool UnitUnknown(const TreeAttrRow &a) {
	return a.has_value && a.dimension.empty() && a.unit.empty() && a.category.empty();
}

//! A value list "X[0]", "X[1]" is one property X with several values.
bool ListMember(const std::string &attr_id, std::string &base) {
	auto open = attr_id.rfind('[');
	if (open == std::string::npos || attr_id.back() != ']' || open == 0) {
		return false;
	}
	for (size_t i = open + 1; i + 1 < attr_id.size(); i++) {
		if (!std::isdigit(static_cast<unsigned char>(attr_id[i]))) {
			return false;
		}
	}
	base = attr_id.substr(0, open);
	return true;
}

//! The value part of a Property or Range for one attribute.
Json ValueElement(const TreeAttrRow &a) {
	Json e;
	if (!a.category.empty() && !a.has_value) {
		bool boolean = (a.category == "true" || a.category == "false") &&
		               (a.value_text.empty() || a.value_text == "true" || a.value_text == "false" ||
		                a.value_text == "1" || a.value_text == "0");
		e["modelType"] = "Property";
		e["valueType"] = boolean ? "xs:boolean" : "xs:string";
		e["value"] = boolean || !IsIdentifier(a.category) || a.value_text.empty() ? a.category : a.value_text;
		if (!boolean && IsIdentifier(a.category)) {
			e["valueId"] = Ref(a.category);
		}
		e["embeddedDataSpecifications"] =
		    Iec61360(a, a.bound == "at_least" ? "min" : a.bound == "at_most" ? "max" : a.bound == "equals" ? "nom" : "");
		return e;
	}
	bool nominal = false;
	if (!a.has_value) {
		e = Json {{"modelType", "Property"}, {"valueType", "xs:double"}}; // stated without a value: a gap
	} else if (a.bound == "at_least") {
		e = Json {{"modelType", "Range"}, {"valueType", "xs:double"}, {"min", Number(a.lo)}};
	} else if (a.bound == "at_most") {
		e = Json {{"modelType", "Range"}, {"valueType", "xs:double"}, {"max", Number(a.hi)}};
	} else if (a.bound == "within" || a.lo != a.hi) {
		e = Json {{"modelType", "Range"}, {"valueType", "xs:double"}, {"min", Number(a.lo)}, {"max", Number(a.hi)}};
	} else {
		nominal = a.bound == "equals";
		e = Json {{"modelType", "Property"}, {"valueType", "xs:double"}, {"value", Number(a.lo)}};
	}
	if (!UnitUnknown(a)) {
		e["embeddedDataSpecifications"] = Iec61360(a, nominal ? "nom" : "");
	}
	return e;
}

//! Ordered element map: idShort -> element, keeping insertion order and unique idShorts.
class Siblings {
public:
	std::string Unique(const std::string &raw) {
		auto id = AasIdShort(raw);
		auto base = id;
		for (int n = 2; taken.count(id); n++) {
			auto suffix = "_" + std::to_string(n);
			id = base.substr(0, AAS_ID_SHORT_MAX - suffix.size()) + suffix;
		}
		taken.insert(id);
		return id;
	}

private:
	std::unordered_set<std::string> taken;
};

class TreeWriter {
public:
	TreeWriter(const std::vector<TreeNodeRow> &tree_nodes, Json &env) : env(env) {
		for (size_t i = 0; i < tree_nodes.size(); i++) {
			auto &n = tree_nodes[i];
			nodes.push_back(&n);
			by_id[n.node_id] = &n;
			for (auto &a : n.attrs) {
				if (a.role == "required" || a.role == "provided") {
					role_counts[a.role]++;
				}
			}
		}
		for (auto n : nodes) {
			if (!n->parent_id.empty() && by_id.count(n->parent_id)) {
				children[n->parent_id].push_back(n);
			}
		}
		// the majority role of the tree, for items without attributes (ties: required)
		auto required = role_counts["required"], provided = role_counts["provided"];
		tree_role = required + provided == 0 ? "" : required >= provided ? "required" : "provided";
	}

	void Write() {
		const TreeNodeRow *root = nullptr;
		for (auto n : nodes) {
			if (n->parent_id.empty() || !by_id.count(n->parent_id)) {
				root = n;
				break;
			}
		}
		if (!root) {
			throw InvalidInputException("trees_to_aas: tree '%s' has no root (every node has a parent in the tree)",
			                            nodes.empty() ? std::string() : nodes[0]->tree_id);
		}
		auto shell_id = root->tree_id;
		Json shell {{"modelType", "AssetAdministrationShell"},
		            {"idShort", AasIdShort(root->label.empty() ? root->tree_id : root->label)},
		            {"id", shell_id},
		            {"assetInformation", {{"assetKind", "Type"}, {"globalAssetId", shell_id + "/asset"}}},
		            {"submodels", Json::array()}};
		if (!root->label.empty()) {
			shell["displayName"] = LangText(root->label, 128);
		}
		Siblings submodel_ids;
		for (auto sm_node : children[root->node_id]) {
			auto id_short = submodel_ids.Unique(Segment(*root, *sm_node, true));
			auto sm_id = shell_id + "/submodels/" + id_short;
			submodel_id[sm_node->node_id] = sm_id;
			Json sm {{"modelType", "Submodel"}, {"idShort", id_short}, {"id", sm_id}, {"kind", "Instance"}};
			if (!sm_node->class_id.empty()) {
				sm["semanticId"] = Ref(sm_node->class_id);
			}
			path_of[sm_node->node_id] = {};
			auto elements = Json::array();
			Siblings siblings;
			WriteAttributes(*sm_node, elements, siblings);
			WriteChildren(*sm_node, *sm_node, elements, siblings);
			sm["submodelElements"] = elements;
			pending_submodels.push_back(std::move(sm));
			shell["submodels"].push_back(
			    {{"type", "ModelReference"}, {"keys", Json::array({{{"type", "Submodel"}, {"value", sm_id}}})}});
		}
		ResolveCompositions();
		for (auto &sm : pending_submodels) {
			if (sm["submodelElements"].empty()) {
				sm.erase("submodelElements"); // the schema allows no empty lists
			}
			env["submodels"].push_back(std::move(sm));
		}
		if (shell["submodels"].empty()) {
			shell.erase("submodels");
		}
		env["assetAdministrationShells"].push_back(std::move(shell));
		for (auto n : nodes) {
			for (auto &a : n->attrs) {
				if (!a.attr_key.empty() && !UnitUnknown(a)) {
					keys.emplace(a.attr_key, &a);
				}
			}
		}
	}

	std::map<std::string, const TreeAttrRow *> keys; // attribute keys -> first attribute (for CDs)

private:
	Json &env;
	std::vector<const TreeNodeRow *> nodes;
	std::unordered_map<std::string, const TreeNodeRow *> by_id;
	std::unordered_map<std::string, std::vector<const TreeNodeRow *>> children;
	std::map<std::string, int> role_counts;
	std::string tree_role;
	std::vector<Json> pending_submodels;
	std::unordered_map<std::string, std::string> submodel_id;                 // level-1 node -> submodel id
	std::unordered_map<std::string, std::vector<std::string>> path_of;       // node -> idShort path in its submodel
	std::unordered_map<std::string, std::string> submodel_of;                 // node -> its submodel's node id
	struct Composition {
		const TreeNodeRow *composite;
		const TreeNodeRow *part;
	};
	std::vector<Composition> compositions;

	//! The idShort of a node below parent: the rest of its node id after the parent's (AAS paths),
	//! else its last path segment.
	static std::string Segment(const TreeNodeRow &parent, const TreeNodeRow &n, bool top) {
		auto &id = n.node_id;
		auto prefix = parent.node_id + ".";
		if (!top && id.size() > prefix.size() && id.compare(0, prefix.size(), prefix) == 0) {
			auto rest = id.substr(prefix.size());
			if (rest.find('.') == std::string::npos && rest.find('[') == std::string::npos) {
				return rest;
			}
		}
		auto cut = id.find_last_of('.');
		return cut == std::string::npos ? id : id.substr(cut + 1);
	}

	bool IsCapability(const TreeNodeRow &n) const {
		if (n.node_kind != "item" && !HasDetachedParts(n)) {
			return false;
		}
		for (auto &a : n.attrs) {
			if (a.role == "required" || a.role == "provided") {
				return true;
			}
		}
		// an item without attributes keeps its class only as a Capability (a collection without values
		// reads back as a container)
		return n.attrs.empty() && (!tree_role.empty() || !n.class_id.empty() || n.node_kind == "item");
	}

	//! Children whose node ids do not lie below the node: ComposedOf parts written next to it.
	bool HasDetachedParts(const TreeNodeRow &n) const {
		auto it = children.find(n.node_id);
		if (it == children.end()) {
			return false;
		}
		for (auto c : it->second) {
			if (c->node_id.compare(0, n.node_id.size() + 1, n.node_id + ".") != 0) {
				return true;
			}
		}
		return false;
	}

	std::string RoleOf(const TreeNodeRow &n) const {
		for (auto &a : n.attrs) {
			if (a.role == "required" || a.role == "provided") {
				return a.role;
			}
		}
		return tree_role;
	}

	static bool AllListIndices(const TreeNodeRow &parent, const std::vector<const TreeNodeRow *> &kids) {
		if (kids.empty()) {
			return false;
		}
		for (auto c : kids) {
			auto prefix = parent.node_id + "[";
			if (c->node_id.compare(0, prefix.size(), prefix) != 0 || c->node_id.back() != ']') {
				return false;
			}
		}
		return true;
	}

	static void AddCount(const TreeNodeRow &n, Json &element) {
		if (n.count != 1) {
			element["qualifiers"].push_back(Qualifier("anofox:Count", "xs:double", Number(n.count)));
		}
	}

	//! Children of n written into elements; detached ComposedOf parts go next to their composite.
	void WriteChildren(const TreeNodeRow &n, const TreeNodeRow &submodel, Json &elements, Siblings &siblings,
	                   bool list = false) {
		auto it = children.find(n.node_id);
		if (it == children.end()) {
			return;
		}
		for (auto c : it->second) {
			bool detached = c->node_id.compare(0, n.node_id.size() + 1, n.node_id + ".") != 0 &&
			                c->node_id.compare(0, n.node_id.size() + 1, n.node_id + "[") != 0;
			if (detached && IsCapability(n) && IsCapability(*c)) {
				compositions.push_back({&n, c}); // written next to n by the caller of n
				continue;
			}
			elements.push_back(WriteNode(*c, n, submodel, siblings, list));
		}
	}

	Json WriteNode(const TreeNodeRow &n, const TreeNodeRow &parent, const TreeNodeRow &submodel, Siblings &siblings,
	               bool in_list) {
		auto id_short = in_list ? std::string() : siblings.Unique(Segment(parent, n, false));
		auto parent_path = path_of[parent.node_id];
		auto path = parent_path;
		path.push_back(in_list ? std::to_string(path.size()) : id_short);
		path_of[n.node_id] = path;
		submodel_of[n.node_id] = submodel.node_id;
		Json e;
		Siblings inner;
		if (IsCapability(n)) {
			e = WriteCapability(n, id_short, submodel);
			// a capability's children are its ComposedOf parts: written next to it, with relations
			for (auto c : children[n.node_id]) {
				compositions.push_back({&n, c});
			}
		} else {
			auto &kids = children[n.node_id];
			bool list = AllListIndices(n, kids);
			if (list) {
				std::stable_sort(kids.begin(), kids.end(), [](const TreeNodeRow *a, const TreeNodeRow *b) {
					return ListIndex(a->node_id) < ListIndex(b->node_id);
				});
			}
			e = Json {{"modelType", list ? "SubmodelElementList" : "SubmodelElementCollection"}};
			if (!in_list) {
				e["idShort"] = id_short;
			}
			if (!n.class_id.empty() && n.node_kind == "item") {
				e["semanticId"] = Ref(n.class_id);
			}
			if (!n.label.empty()) {
				e["displayName"] = LangText(n.label, 128);
			}
			e["qualifiers"] = Json::array();
			if (n.node_kind == "variant_group") {
				e["qualifiers"].push_back(Qualifier("anofox:VariantGroup", "xs:boolean", "true"));
			}
			AddCount(n, e);
			if (e["qualifiers"].empty()) {
				e.erase("qualifiers");
			}
			if (list) {
				e["typeValueListElement"] = "SubmodelElementCollection";
			}
			auto value = Json::array();
			WriteAttributes(n, value, inner);
			WriteChildren(n, submodel, value, inner, list);
			if (list) {
				// list members: their path segment is their index
				for (size_t k = 0; k < kids.size(); k++) {
					auto member = path;
					member.push_back(std::to_string(k));
					path_of[kids[k]->node_id] = member;
				}
			}
			if (!value.empty()) {
				e["value"] = value;
			} else if (list) {
				e["modelType"] = "SubmodelElementCollection";
				e.erase("typeValueListElement");
			}
		}
		return e;
	}

	//! Generic attributes: Property / Range elements, value lists as SubmodelElementLists.
	void WriteAttributes(const TreeNodeRow &n, Json &elements, Siblings &siblings) {
		std::map<std::string, std::vector<const TreeAttrRow *>> lists;
		std::vector<std::string> order;
		for (auto &a : n.attrs) {
			std::string base;
			if (ListMember(a.attr_id, base)) {
				if (!lists.count(base)) {
					order.push_back("L" + base);
				}
				lists[base].push_back(&a);
			} else {
				order.push_back("A" + a.attr_id);
			}
		}
		for (auto &entry : order) {
			auto name = entry.substr(1);
			if (entry[0] == 'L') {
				auto &members = lists[name];
				std::stable_sort(members.begin(), members.end(), [](const TreeAttrRow *a, const TreeAttrRow *b) {
					return ListIndex(a->attr_id) < ListIndex(b->attr_id); // X[10] after X[9], whatever the input order
				});
				Json list {{"modelType", "SubmodelElementList"}, {"idShort", siblings.Unique(name)}};
				if (!members[0]->attr_key.empty()) {
					list["semanticId"] = Ref(members[0]->attr_key);
				}
				auto first = ValueElement(*members[0]);
				list["typeValueListElement"] = first["modelType"];
				list["valueTypeListElement"] = first["valueType"];
				auto value = Json::array();
				for (auto m : members) {
					auto v = ValueElement(*m);
					value.push_back(v);
				}
				list["value"] = value;
				elements.push_back(list);
				continue;
			}
			for (auto &a : n.attrs) {
				if (a.attr_id != name) {
					continue;
				}
				auto v = ValueElement(a);
				Json e {{"modelType", v["modelType"]}, {"idShort", siblings.Unique(a.attr_id)}};
				if (!a.attr_key.empty()) {
					e["semanticId"] = Ref(a.attr_key);
				}
				for (auto &field : v.items()) {
					if (field.key() != "modelType") {
						e[field.key()] = field.value();
					}
				}
				elements.push_back(e);
			}
		}
	}

	//! An IDTA-02020 CapabilityContainer: Capability (class, role), PropertySet, relations.
	Json WriteCapability(const TreeNodeRow &n, const std::string &id_short, const TreeNodeRow &submodel) {
		auto cap_id = id_short;
		const std::string suffix = "Container";
		if (cap_id.size() > suffix.size() + 1 && cap_id.compare(cap_id.size() - suffix.size(), suffix.size(), suffix) == 0) {
			cap_id = cap_id.substr(0, cap_id.size() - suffix.size());
		} else {
			cap_id = AasIdShort(cap_id + "Capability");
		}
		auto role = RoleOf(n);
		Json capability {{"modelType", "Capability"}, {"idShort", cap_id}};
		if (!n.label.empty()) {
			capability["displayName"] = LangText(n.label, 128);
		}
		capability["semanticId"] = Ref(std::string(CD) + "Capability/1/0");
		if (!n.class_id.empty()) {
			capability["supplementalSemanticIds"] = Json::array({Ref(n.class_id)});
		}
		auto qualifiers = Json::array();
		for (auto kind : {"Required", "Offered", "NotAssigned"}) {
			bool on = (role == "required" && std::string(kind) == "Required") ||
			          (role == "provided" && std::string(kind) == "Offered") ||
			          (role.empty() && std::string(kind) == "NotAssigned");
			auto q = Qualifier(kind, "xs:boolean", on ? "true" : "false");
			q["kind"] = "ValueQualifier";
			q["semanticId"] = Ref(std::string(CD) + "CapabilityRoleQualifier/" + kind + "/1/0");
			qualifiers.push_back(q);
		}
		capability["qualifiers"] = qualifiers;
		auto value = Json::array({capability});
		if (!n.attrs.empty()) {
			value.push_back(PropertySet(n));
		}
		Json container {{"modelType", "SubmodelElementCollection"},
		                {"idShort", id_short},
		                {"semanticId", Ref(std::string(CD) + "CapabilityContainer/1/0")}};
		container["qualifiers"] = Json::array();
		AddCount(n, container);
		if (container["qualifiers"].empty()) {
			container.erase("qualifiers");
		}
		container["value"] = value;
		capability_idshort[n.node_id] = cap_id;
		return container;
	}

	std::unordered_map<std::string, std::string> capability_idshort;

	Json PropertySet(const TreeNodeRow &n) {
		Siblings siblings;
		std::map<std::string, std::vector<const TreeAttrRow *>> groups;
		std::vector<std::string> order;
		for (auto &a : n.attrs) {
			std::string base;
			auto name = ListMember(a.attr_id, base) ? base : a.attr_id;
			if (!groups.count(name)) {
				order.push_back(name);
			}
			groups[name].push_back(&a);
		}
		auto containers = Json::array();
		for (auto &name : order) {
			auto &members = groups[name];
			std::stable_sort(members.begin(), members.end(), [](const TreeAttrRow *a, const TreeAttrRow *b) {
				return ListIndex(a->attr_id) < ListIndex(b->attr_id);
			});
			Json inner;
			std::string base;
			if (members.size() > 1 || ListMember(members[0]->attr_id, base)) {
				auto first = ValueElement(*members[0]);
				inner = Json {{"modelType", "SubmodelElementList"},
				              {"idShort", "PropertySubmodelList"},
				              {"semanticId", Ref(std::string(CPT) + "SubmodelElementList/1/0")}};
				if (!members[0]->attr_key.empty()) {
					inner["supplementalSemanticIds"] = Json::array({Ref(members[0]->attr_key)});
				}
				inner["typeValueListElement"] = first["modelType"];
				inner["valueTypeListElement"] = first["valueType"];
				auto value = Json::array();
				for (auto m : members) {
					value.push_back(ValueElement(*m));
				}
				inner["value"] = value;
			} else {
				auto &a = *members[0];
				auto v = ValueElement(a);
				bool range = v["modelType"] == "Range";
				inner = Json {{"modelType", v["modelType"]},
				              {"idShort", range ? "PropertyRange" : "PropertyProperty"},
				              {"semanticId", Ref(std::string(CPT) + (range ? "Range/1/0" : "Property/1/0"))}};
				if (!a.attr_key.empty()) {
					inner["supplementalSemanticIds"] = Json::array({Ref(a.attr_key)});
				}
				for (auto &field : v.items()) {
					if (field.key() != "modelType") {
						inner[field.key()] = field.value();
					}
				}
			}
			containers.push_back({{"modelType", "SubmodelElementCollection"},
			                      {"idShort", siblings.Unique(name)},
			                      {"semanticId", Ref(std::string(CD) + "PropertyContainer/1/0")},
			                      {"value", Json::array({inner})}});
		}
		return {{"modelType", "SubmodelElementCollection"},
		        {"idShort", "PropertySet"},
		        {"semanticId", Ref(std::string(CD) + "PropertySet/1/0")},
		        {"value", containers}};
	}

	//! ModelReference to a capability: Submodel, the collections of its path, the Capability.
	Json CapabilityReference(const TreeNodeRow &n) {
		auto keys = Json::array({{{"type", "Submodel"}, {"value", submodel_id[submodel_of[n.node_id]]}}});
		for (auto &segment : path_of[n.node_id]) {
			keys.push_back({{"type", "SubmodelElementCollection"}, {"value", segment}});
		}
		keys.push_back({{"type", "Capability"}, {"value", capability_idshort[n.node_id]}});
		return {{"type", "ModelReference"}, {"keys", keys}};
	}

	//! Writes each ComposedOf part next to its composite and adds the CapabilityComposedOf relations.
	void ResolveCompositions() {
		// parts may be composites themselves: process until no new compositions appear
		for (size_t done = 0; done < compositions.size();) {
			std::map<std::string, std::vector<const TreeNodeRow *>> parts_of;
			for (; done < compositions.size(); done++) {
				parts_of[compositions[done].composite->node_id].push_back(compositions[done].part);
			}
			WriteParts(parts_of);
		}
	}

	void WriteParts(const std::map<std::string, std::vector<const TreeNodeRow *>> &parts_of) {
		for (auto &entry : parts_of) {
			auto composite = by_id[entry.first];
			auto &holder_path = path_of[composite->node_id];
			auto sm_node = by_id[submodel_of[composite->node_id]];
			Json *holder = FindElements(sm_node, std::vector<std::string>(holder_path.begin(), holder_path.end() - 1));
			if (!holder) {
				continue;
			}
			auto parent_node = FindNodeByPath(sm_node, std::vector<std::string>(holder_path.begin(), holder_path.end() - 1));
			Siblings siblings;
			for (auto &el : *holder) {
				if (el.contains("idShort")) {
					siblings.Unique(el["idShort"].get<std::string>());
				}
			}
			auto relations = Json::array();
			for (auto part : entry.second) {
				holder->push_back(WriteNode(*part, *parent_node, *sm_node, siblings, false));
			}
			for (size_t i = 0; i < entry.second.size(); i++) {
				Json relation {{"modelType", "RelationshipElement"},
				               {"idShort", "ComposedOf" + std::to_string(i)},
				               {"semanticId", Ref(std::string(CD) + "CapabilityComposedOf/1/0")},
				               {"first", CapabilityReference(*composite)},
				               {"second", CapabilityReference(*entry.second[i])}};
				relations.push_back(std::move(relation));
			}
			Json *container = FindElement(sm_node, holder_path);
			if (container) {
				(*container)["value"].push_back(
				    {{"modelType", "SubmodelElementCollection"},
				     {"idShort", "CapabilityRelations"},
				     {"semanticId", Ref(std::string(CD) + "CapabilityRelations/1/0")},
				     {"value", Json::array({{{"modelType", "SubmodelElementCollection"},
				                             {"idShort", "ComposedOfSet"},
				                             {"semanticId", Ref(std::string(CD) + "ComposedOfSet/1/0")},
				                             {"value", Json::array({{{"modelType", "SubmodelElementCollection"},
				                                                     {"idShort", "ComposedOfContainer"},
				                                                     {"semanticId", Ref(std::string(CD) + "ComposedOfContainer/1/0")},
				                                                     {"value", relations}}})}}})}});
			}
		}
	}

	Json *SubmodelJson(const TreeNodeRow *sm_node) {
		for (auto &sm : pending_submodels) {
			if (sm["id"] == submodel_id[sm_node->node_id]) {
				return &sm;
			}
		}
		return nullptr;
	}

	//! The element at an idShort path, or nullptr.
	Json *FindElement(const TreeNodeRow *sm_node, const std::vector<std::string> &path) {
		auto sm = SubmodelJson(sm_node);
		if (!sm || path.empty()) {
			return nullptr;
		}
		Json *list = &(*sm)["submodelElements"];
		Json *found = nullptr;
		for (auto &segment : path) {
			found = nullptr;
			for (auto &el : *list) {
				if (el.contains("idShort") && el["idShort"] == segment) {
					found = &el;
					break;
				}
			}
			if (!found || !found->contains("value")) {
				return found;
			}
			list = &(*found)["value"];
		}
		return found;
	}

	//! The element list at an idShort path (the submodel's elements for an empty path).
	Json *FindElements(const TreeNodeRow *sm_node, const std::vector<std::string> &path) {
		if (path.empty()) {
			auto sm = SubmodelJson(sm_node);
			return sm ? &(*sm)["submodelElements"] : nullptr;
		}
		auto el = FindElement(sm_node, path);
		return el && el->contains("value") ? &(*el)["value"] : nullptr;
	}

	const TreeNodeRow *FindNodeByPath(const TreeNodeRow *sm_node, const std::vector<std::string> &path) {
		for (auto &entry : path_of) {
			if (entry.second == path && submodel_of[entry.first] == sm_node->node_id) {
				return by_id[entry.first];
			}
		}
		return sm_node;
	}
};

} // namespace

std::string AasIdShort(const std::string &raw) {
	// German umlauts and ß are transliterated, other non-ASCII characters become '_'
	static const std::pair<const char *, const char *> TRANSLIT[] = {
	    {"\xc3\xa4", "ae"}, {"\xc3\xb6", "oe"}, {"\xc3\xbc", "ue"}, {"\xc3\x84", "Ae"},
	    {"\xc3\x96", "Oe"}, {"\xc3\x9c", "Ue"}, {"\xc3\x9f", "ss"}};
	std::string text = raw;
	for (auto &t : TRANSLIT) {
		for (size_t pos = text.find(t.first); pos != std::string::npos; pos = text.find(t.first, pos)) {
			text.replace(pos, std::strlen(t.first), t.second);
		}
	}
	std::string out;
	bool upper_next = false;
	for (char c : text) {
		if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
			out += upper_next && std::isalpha(static_cast<unsigned char>(c))
			           ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
			           : c;
			upper_next = false;
		} else if (c == ' ') {
			upper_next = true;
		} else if (!out.empty() && out.back() != '_') {
			out += '_';
		}
	}
	if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0]))) {
		out = "n" + out;
	}
	out = out.substr(0, AAS_ID_SHORT_MAX); // cut first, then make the end valid
	while (out.size() > 1 && out.back() == '-') {
		out.pop_back();
	}
	if (out.size() < 2) {
		out += "_";
	}
	return out;
}

std::string WriteTreesAsAas(const std::vector<TreeNodeRow> &nodes, const std::vector<CatalogueEntry> &catalogue) {
	Json env {{"assetAdministrationShells", Json::array()}, {"submodels", Json::array()},
	          {"conceptDescriptions", Json::array()}};
	std::map<std::string, std::vector<TreeNodeRow>> trees;
	std::vector<std::string> order;
	for (auto &n : nodes) {
		if (!trees.count(n.tree_id)) {
			order.push_back(n.tree_id);
		}
		trees[n.tree_id].push_back(n);
	}
	std::map<std::string, const TreeAttrRow *> keys;
	std::vector<TreeWriter> writers;
	writers.reserve(order.size());
	for (auto &tree_id : order) {
		writers.emplace_back(trees[tree_id], env);
		writers.back().Write();
		keys.insert(writers.back().keys.begin(), writers.back().keys.end());
	}
	std::unordered_set<std::string> written;
	for (auto &entry : keys) {
		auto &a = *entry.second;
		written.insert(entry.first);
		Json cd {{"modelType", "ConceptDescription"}, {"id", entry.first}};
		if (!a.name.empty()) {
			cd["idShort"] = AasIdShort(a.name);
		}
		auto content = Json {{"modelType", "DataSpecificationIec61360"},
		                     {"preferredName", LangText(a.name.empty() ? a.attr_id : a.name, 255)}};
		if (!a.unit.empty() && a.unit != "1") {
			content["unit"] = a.unit;
		}
		cd["embeddedDataSpecifications"] =
		    Json::array({{{"dataSpecification", Ref(IEC61360)}, {"dataSpecificationContent", content}}});
		env["conceptDescriptions"].push_back(cd);
	}
	for (auto &c : catalogue) {
		if (c.class_id.empty() || written.count(c.class_id)) {
			continue;
		}
		written.insert(c.class_id);
		auto content = Json {{"modelType", "DataSpecificationIec61360"},
		                     {"preferredName", LangText(c.label.empty() ? c.class_id : c.label, 255)}};
		if (!c.definition.empty()) {
			content["definition"] = LangText(c.definition, 1023);
		}
		Json cd {{"modelType", "ConceptDescription"}, {"id", c.class_id}};
		if (!c.label.empty()) {
			cd["idShort"] = AasIdShort(c.label);
		}
		cd["embeddedDataSpecifications"] =
		    Json::array({{{"dataSpecification", Ref(IEC61360)}, {"dataSpecificationContent", content}}});
		env["conceptDescriptions"].push_back(cd);
	}
	for (auto member : {"assetAdministrationShells", "submodels", "conceptDescriptions"}) {
		if (env[member].empty()) {
			env.erase(member); // the schema allows no empty lists
		}
	}
	return env.dump(2);
}

//! A metric of the trace: written without a value when the trace had none (NaN), never as 0.
std::string Metric(double v) {
	return std::isnan(v) ? std::string() : Number(v);
}

std::string WriteCoverageResult(const std::string &query_id, const std::string &candidate_id, double coverage,
                                const std::vector<TraceRequirementEntry> &requirements, const std::string &id_prefix) {
	auto sem = [](const std::string &name) { return Ref(std::string(ANOFOX) + "CoverageResult/" + name + "/1/0"); };
	auto prop = [&](const std::string &id_short, const std::string &value_type, const std::string &value) {
		Json p {{"modelType", "Property"}, {"idShort", id_short}, {"semanticId", sem(id_short)}, {"valueType", value_type}};
		if (!value.empty()) {
			p["value"] = value;
		}
		if (value_type == "xs:double" || value_type == "xs:int") {
			// a dimensionless number: with its own dictionary entry it reads back without findings
			p["embeddedDataSpecifications"] = Json::array(
			    {{{"dataSpecification", Ref(IEC61360)},
			      {"dataSpecificationContent",
			       {{"modelType", "DataSpecificationIec61360"}, {"preferredName", LangText(id_short, 255)}, {"unit", "1"}}}}});
		}
		return p;
	};
	int64_t gaps = 0;
	auto reqs = Json::array();
	for (auto &r : requirements) {
		gaps += r.is_gap ? 1 : 0;
		auto attrs = Json::array();
		for (auto &a : r.attrs) {
			auto value = Json::array({prop("AttributeId", "xs:string", a.attr_id), prop("AttributeName", "xs:string", a.attr_name),
			                          prop("Required", "xs:string", a.required), prop("Offered", "xs:string", a.offered),
			                          prop("Rule", "xs:string", a.rule), prop("NameRule", "xs:string", a.name_rule),
			                          prop("BoundSource", "xs:string", a.bound_source),
			                          prop("Score", "xs:double", a.has_score ? Metric(a.score) : "")});
			attrs.push_back({{"modelType", "SubmodelElementCollection"}, {"semanticId", sem("Attribute")}, {"value", value}});
		}
		auto value = Json::array({prop("RequirementNode", "xs:string", r.req_node_id),
		                          prop("MatchedNode", "xs:string", r.matched_node_id),
		                          prop("MatchedPath", "xs:string", r.matched_path),
		                          prop("LabelSimilarity", "xs:double", Metric(r.label_sim)),
		                          prop("LabelRule", "xs:string", r.label_rule),
		                          prop("CountNeeded", "xs:double", Metric(r.count_needed)),
		                          prop("CountAvailable", "xs:double", Metric(r.count_available)),
		                          prop("RequirementScore", "xs:double", Metric(r.req_score)),
		                          prop("IsGap", "xs:boolean", r.is_gap ? "true" : "false")});
		if (!attrs.empty()) {
			value.push_back({{"modelType", "SubmodelElementList"},
			                 {"idShort", "Attributes"},
			                 {"semanticId", sem("Attributes")},
			                 {"semanticIdListElement", sem("Attribute")},
			                 {"typeValueListElement", "SubmodelElementCollection"},
			                 {"value", attrs}});
		}
		reqs.push_back({{"modelType", "SubmodelElementCollection"}, {"semanticId", sem("Requirement")}, {"value", value}});
	}
	auto elements = Json::array({prop("Query", "xs:string", query_id), prop("Candidate", "xs:string", candidate_id),
	                             prop("Coverage", "xs:double", Metric(coverage)),
	                             prop("GapCount", "xs:int", std::to_string(gaps))});
	if (!reqs.empty()) {
		elements.push_back({{"modelType", "SubmodelElementList"},
		                    {"idShort", "Requirements"},
		                    {"semanticId", sem("Requirements")},
		                    {"semanticIdListElement", sem("Requirement")},
		                    {"typeValueListElement", "SubmodelElementCollection"},
		                    {"value", reqs}});
	}
	Json sm {{"modelType", "Submodel"},
	         {"idShort", "CoverageResult"},
	         {"id", (id_prefix.empty() ? std::string(ANOFOX) + "results/" : id_prefix) + AasBase64Url(query_id) + "/" +
	                    AasBase64Url(candidate_id)},
	         {"kind", "Instance"},
	         {"semanticId", Ref(std::string(ANOFOX) + "CoverageResult/1/0")},
	         {"submodelElements", elements}};
	return sm.dump(2);
}

} // namespace anofox
} // namespace duckdb

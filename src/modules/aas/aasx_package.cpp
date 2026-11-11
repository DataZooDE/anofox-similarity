// AASX package reader (IDTA-01005, OPC zip). Follows the package relationships
//   /_rels/.rels  --aasx-origin-->  /aasx/aasx-origin  --aas-spec-->  spec part(s)
// and reads every spec part as XML or JSON. Supplementary files (images, PDFs) are skipped.
#include "modules/aas/aasx_zip.hpp" // first: see the header

#include "modules/aas/aas_model.hpp"

#include "duckdb/common/exception.hpp"

#include <tinyxml2.h>

#include <cctype>
#include <cstring>

namespace duckdb {
namespace anofox {

namespace {

//! A zip archive in memory. Owns the miniz reader state.
class ZipArchive {
public:
	explicit ZipArchive(const std::string &bytes) {
		std::memset(&zip, 0, sizeof(zip));
		if (!aasx_zip::mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
			throw InvalidInputException("AASX: not a readable zip package");
		}
	}
	~ZipArchive() {
		aasx_zip::mz_zip_reader_end(&zip);
	}
	ZipArchive(const ZipArchive &) = delete;
	ZipArchive &operator=(const ZipArchive &) = delete;

	bool Read(const std::string &name, std::string &out) {
		int index = aasx_zip::mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
		if (index < 0) {
			return false;
		}
		size_t size = 0;
		void *data = aasx_zip::mz_zip_reader_extract_to_heap(&zip, static_cast<aasx_zip::mz_uint>(index), &size, 0);
		if (!data) {
			throw InvalidInputException("AASX: cannot extract '%s'", name);
		}
		out.assign(static_cast<const char *>(data), size);
		aasx_zip::mz_free(data);
		return true;
	}

	std::vector<std::string> Names() {
		std::vector<std::string> names;
		auto n = aasx_zip::mz_zip_reader_get_num_files(&zip);
		for (aasx_zip::mz_uint i = 0; i < n; i++) {
			char name[1024];
			aasx_zip::mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
			names.emplace_back(name);
		}
		return names;
	}

private:
	aasx_zip::mz_zip_archive zip;
};

bool EndsWith(const std::string &s, const std::string &suffix) {
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string Lower(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

//! A part name inside the zip from a relationship target, relative to the source part's folder.
std::string PartName(const std::string &source_dir, const std::string &target) {
	if (!target.empty() && target[0] == '/') {
		return target.substr(1);
	}
	return source_dir + target;
}

std::string Folder(const std::string &part) {
	auto slash = part.rfind('/');
	return slash == std::string::npos ? std::string() : part.substr(0, slash + 1);
}

//! The relationship part of a part: a/b/c.xml -> a/b/_rels/c.xml.rels; the package: _rels/.rels.
std::string RelsOf(const std::string &part) {
	auto slash = part.rfind('/');
	auto dir = slash == std::string::npos ? std::string() : part.substr(0, slash + 1);
	auto file = slash == std::string::npos ? part : part.substr(slash + 1);
	return dir + "_rels/" + file + ".rels";
}

//! Targets of the relationships in a .rels part whose type ends with type_suffix.
std::vector<std::string> Targets(ZipArchive &zip, const std::string &rels_part, const std::string &source_dir,
                                 const char *type_suffix) {
	std::vector<std::string> out;
	std::string xml;
	if (!zip.Read(rels_part, xml)) {
		return out;
	}
	tinyxml2::XMLDocument doc;
	if (doc.Parse(xml.data(), xml.size()) != tinyxml2::XML_SUCCESS || !doc.RootElement()) {
		throw InvalidInputException("AASX: '%s' is not valid XML", rels_part);
	}
	for (auto r = doc.RootElement()->FirstChildElement(); r; r = r->NextSiblingElement()) {
		auto type = r->Attribute("Type");
		auto target = r->Attribute("Target");
		if (type && target && EndsWith(type, type_suffix)) {
			out.push_back(PartName(source_dir, target));
		}
	}
	return out;
}

AasEnvironment ReadSpecPart(ZipArchive &zip, const std::string &part) {
	std::string content;
	if (!zip.Read(part, content)) {
		throw InvalidInputException("AASX: the package names the spec part '%s' but does not contain it", part);
	}
	return EndsWith(Lower(part), ".json") ? ReadAasJson(content) : ReadAasXml(content);
}

} // namespace

AasEnvironment ReadAasx(const std::string &bytes) {
	ZipArchive zip(bytes);
	std::vector<std::string> specs;
	for (auto &origin : Targets(zip, "_rels/.rels", "", "/aasx-origin")) {
		auto parts = Targets(zip, RelsOf(origin), Folder(origin), "/aas-spec");
		specs.insert(specs.end(), parts.begin(), parts.end());
	}
	if (specs.empty()) {
		// packages written without relationships: any *.aas.xml / *.aas.json part
		for (auto &name : zip.Names()) {
			auto lower = Lower(name);
			if (EndsWith(lower, ".aas.xml") || EndsWith(lower, ".aas.json")) {
				specs.push_back(name);
			}
		}
	}
	if (specs.empty()) {
		throw InvalidInputException("AASX: the package has no AAS spec part (aasx-origin -> aas-spec)");
	}
	AasEnvironment env;
	for (auto &part : specs) {
		env.Append(ReadSpecPart(zip, part));
	}
	return env;
}

void AasEnvironment::Append(AasEnvironment &&other) {
	for (auto &s : other.shells) {
		shells.push_back(std::move(s));
	}
	for (auto &s : other.submodels) {
		submodels.push_back(std::move(s));
	}
	for (auto &c : other.concept_descriptions) {
		concept_descriptions.push_back(std::move(c));
	}
}

bool AasIsContainer(const std::string &model_type) {
	return model_type == "SubmodelElementCollection" || model_type == "SubmodelElementList" || model_type == "Entity";
}

AasEnvironment ReadAas(const std::string &content) {
	size_t i = 0;
	if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
	    static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
		i = 3; // UTF-8 byte order mark
	}
	if (content.size() >= 4 && content.compare(0, 4, "PK\x03\x04") == 0) {
		return ReadAasx(content);
	}
	while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
		i++;
	}
	if (i == content.size()) {
		throw InvalidInputException("AAS: empty input");
	}
	if (content[i] == '<') {
		return ReadAasXml(content);
	}
	if (content[i] == '{' || content[i] == '[') {
		return ReadAasJson(content.substr(i));
	}
	throw InvalidInputException("AAS: expected JSON, XML or an AASX package (zip)");
}

} // namespace anofox
} // namespace duckdb

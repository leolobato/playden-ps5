#include "runtime/burn_bundle.h"

#include "runtime/msi_package.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <vector>

namespace playden::runtime {
namespace {

constexpr std::uint32_t kBurnMagic = 0x00f14300;
constexpr std::size_t kMaxManifestBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxContainers = 64;

std::uint32_t U32(std::string_view bytes, std::size_t at) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t U16(std::string_view bytes, std::size_t at) {
  const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + at);
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& value : out)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return out;
}

// A cabinet that starts at offset and is exactly size bytes long (its header says so).
bool CabinetAt(std::string_view bundle, std::uint64_t offset, std::uint64_t size) {
  return size >= 36 && offset <= bundle.size() && size <= bundle.size() - offset &&
         bundle.compare(offset, 8, std::string_view("MSCF\0\0\0\0", 8)) == 0 && U32(bundle, offset + 8) == size;
}

// The row of the PE section table named .wixburn, or nullopt (not a PE, or no such section).
std::optional<std::uint64_t> WixburnRow(std::string_view bundle) {
  if (bundle.size() < 64 || bundle[0] != 'M' || bundle[1] != 'Z') return std::nullopt;
  const auto pe = U32(bundle, 0x3c);
  if (pe > bundle.size() || bundle.size() - pe < 24 || bundle.compare(pe, 4, std::string_view("PE\0\0", 4)) != 0)
    return std::nullopt;
  const unsigned sections = U16(bundle, pe + 6);
  const unsigned optional_size = U16(bundle, pe + 20);
  const std::uint64_t table = static_cast<std::uint64_t>(pe) + 24 + optional_size;
  if (sections > 96 || table + sections * 40ull > bundle.size()) return std::nullopt;
  for (unsigned index = 0; index < sections; ++index) {
    const auto row = table + index * 40ull;
    if (bundle.substr(row, 8) == std::string_view(".wixburn", 8)) return row;
  }
  return std::nullopt;
}

// The bundle's containers in order: [0] the UX container, then the attached ones.
std::optional<std::vector<std::string_view>> SectionContainers(std::string_view bundle) {
  const auto row = WixburnRow(bundle);
  if (!row) return std::nullopt;
  const std::uint64_t raw = U32(bundle, *row + 20), size = U32(bundle, *row + 16);
  if (raw > bundle.size() || size > bundle.size() - raw || size < 48) return std::nullopt;
  const auto section = bundle.substr(raw, size);
  if (U32(section, 0) != kBurnMagic || U32(section, 40) != 1) return std::nullopt;  // 1: cabinet containers
  const std::uint64_t stub = U32(section, 24), signature = U32(section, 32), signature_size = U32(section, 36);
  const std::uint32_t count = U32(section, 44);
  if (count == 0 || count > kMaxContainers || 48 + count * 4ull > section.size()) return std::nullopt;
  std::vector<std::uint64_t> sizes;
  for (std::uint32_t at = 0; at < count; ++at) sizes.push_back(U32(section, 48 + at * 4));
  if (!CabinetAt(bundle, stub, sizes[0])) return std::nullopt;
  std::vector<std::string_view> out{bundle.substr(stub, sizes[0])};
  if (count == 1) return out;
  // Attached containers follow the engine: after the original signature when the bundle was signed
  // before they were attached (Microsoft's are), right after the UX container otherwise.
  for (const std::uint64_t first : {signature ? signature + signature_size : 0, stub + sizes[0]}) {
    if (!first) continue;
    std::vector<std::string_view> attached;
    std::uint64_t at = first;
    for (std::uint32_t container = 1; container < count && CabinetAt(bundle, at, sizes[container]); ++container) {
      attached.push_back(bundle.substr(at, sizes[container]));
      at += sizes[container];
    }
    if (attached.size() == count - 1) {
      out.insert(out.end(), attached.begin(), attached.end());
      return out;
    }
  }
  return std::nullopt;
}

// Members of a cabinet by name; false when one is missing or the cabinet can't be read.
bool Members(std::string_view cabinet, const std::set<std::string>& names, std::map<std::string, std::string>* out,
             std::string* reason) {
  std::string why;
  const bool read = ExtractCabinet(cabinet, [&](const std::string& name, std::string_view data) {
    if (names.count(name) && !out->count(name)) (*out)[name] = std::string(data);
    return out->size() < names.size();  // stop once everything is in
  }, &why);
  if (out->size() == names.size()) return true;
  *reason = read ? "a file is missing from the bundle's container" : "the bundle's container could not be read: " + why;
  return false;
}

// Without a usable .wixburn section: the cabinets the bundle holds, in file order. The UX container is
// the first whose file "0" is a Burn manifest.
std::vector<std::string_view> ScannedContainers(std::string_view bundle) {
  std::vector<std::string_view> found;
  for (std::size_t at = bundle.find(std::string_view("MSCF\0\0\0\0", 8)); at != std::string_view::npos;) {
    const std::uint64_t size = at + 12 <= bundle.size() ? U32(bundle, at + 8) : 0;
    if (CabinetAt(bundle, at, size)) {
      found.push_back(bundle.substr(at, size));
      at += size;
    } else {
      ++at;
    }
    at = bundle.find(std::string_view("MSCF\0\0\0\0", 8), at);
  }
  for (std::size_t index = 0; index < found.size(); ++index) {
    std::map<std::string, std::string> manifest;
    std::string ignored;
    if (Members(found[index], {"0"}, &manifest, &ignored) && manifest["0"].find("<BurnManifest") != std::string::npos) {
      found.erase(found.begin(), found.begin() + static_cast<std::ptrdiff_t>(index));
      return found;
    }
  }
  return {};
}

// ---- The manifest: just enough XML for its elements and their attributes.

struct Element {
  std::string name;  // "Payload", "/MsiPackage"
  std::map<std::string, std::string> attributes;
  bool closed = false;  // <name ... />
};

std::string Unescape(std::string_view text) {
  static const std::pair<std::string_view, char> kEntities[] = {
      {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
  std::string out;
  for (std::size_t at = 0; at < text.size();) {
    bool replaced = false;
    if (text[at] == '&')
      for (const auto& [entity, value] : kEntities)
        if (text.compare(at, entity.size(), entity) == 0) {
          out += value;
          at += entity.size();
          replaced = true;
          break;
        }
    if (!replaced) out += text[at++];
  }
  return out;
}

bool Space(char value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }

// The manifest's elements in order; false when it isn't well formed enough to read.
bool Elements(std::string_view xml, std::vector<Element>* out) {
  for (std::size_t at = xml.find('<'); at != std::string_view::npos; at = xml.find('<', at)) {
    ++at;
    if (xml.compare(at, 3, "!--") == 0) {
      const auto end = xml.find("-->", at);
      if (end == std::string_view::npos) return false;
      at = end + 3;
      continue;
    }
    if (at < xml.size() && (xml[at] == '?' || xml[at] == '!')) {
      const auto end = xml.find('>', at);
      if (end == std::string_view::npos) return false;
      at = end + 1;
      continue;
    }
    Element element;
    while (at < xml.size() && !Space(xml[at]) && xml[at] != '>' && xml[at] != '/') element.name += xml[at++];
    if (at < xml.size() && xml[at] == '/' && element.name.empty()) {
      element.name = "/";
      for (++at; at < xml.size() && !Space(xml[at]) && xml[at] != '>';) element.name += xml[at++];
    }
    for (;;) {
      while (at < xml.size() && Space(xml[at])) ++at;
      if (at >= xml.size()) return false;
      if (xml[at] == '>') { ++at; break; }
      if (xml[at] == '/') {
        element.closed = true;
        ++at;
        continue;
      }
      std::string key;
      while (at < xml.size() && !Space(xml[at]) && xml[at] != '=' && xml[at] != '>' && xml[at] != '/') key += xml[at++];
      while (at < xml.size() && Space(xml[at])) ++at;
      if (key.empty() || at >= xml.size() || xml[at] != '=') return false;
      ++at;
      while (at < xml.size() && Space(xml[at])) ++at;
      if (at >= xml.size() || (xml[at] != '"' && xml[at] != '\'')) return false;
      const char quote = xml[at++];
      const auto end = xml.find(quote, at);
      if (end == std::string_view::npos) return false;
      element.attributes[key] = Unescape(xml.substr(at, end - at));
      at = end + 1;
    }
    if (element.name.empty()) return false;
    out->push_back(std::move(element));
  }
  return true;
}

std::string Attribute(const Element& element, const char* name) {
  const auto found = element.attributes.find(name);
  return found == element.attributes.end() ? std::string() : found->second;
}

std::string FileName(const std::string& path) {
  const auto slash = path.find_last_of("\\/");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The bundle's containers and its manifest's elements; false with a reason when either can't be read.
bool ReadManifest(std::string_view bundle, std::vector<std::string_view>* containers, std::vector<Element>* elements,
                  std::string* reason) {
  auto found = SectionContainers(bundle);
  if (!found) found = ScannedContainers(bundle);
  if (found->empty()) {
    *reason = "not a setup bundle Playden can read";
    return false;
  }
  *containers = std::move(*found);
  std::map<std::string, std::string> ux;
  if (!Members(containers->front(), {"0"}, &ux, reason)) return false;
  std::string_view manifest = ux["0"];
  if (manifest.size() > kMaxManifestBytes) {
    *reason = "the bundle's manifest is too large";
    return false;
  }
  if (manifest.substr(0, 3) == "\xEF\xBB\xBF") manifest.remove_prefix(3);
  if (!Elements(manifest, elements)) {
    *reason = "the bundle's manifest could not be read";
    return false;
  }
  return true;
}

// The attached containers the manifest lists, by id: their index in the bundle's containers.
std::map<std::string, std::size_t> AttachedContainers(const std::vector<Element>& elements) {
  std::map<std::string, std::size_t> attached;
  for (const auto& element : elements)
    if (element.name == "Container" && Attribute(element, "Attached") == "yes") {
      const auto index = std::strtoul(Attribute(element, "AttachedIndex").c_str(), nullptr, 10);
      if (index > 0) attached[Attribute(element, "Id")] = index;
    }
  return attached;
}

}  // namespace

bool HasBurnSection(std::string_view head) { return WixburnRow(head).has_value(); }

bool ReadBurnMsiPackageIds(std::string_view bundle, std::vector<std::string>* ids, std::string* reason) {
  ids->clear();
  std::vector<std::string_view> containers;
  std::vector<Element> elements;
  if (!ReadManifest(bundle, &containers, &elements, reason)) return false;
  // A bundle cut short keeps its manifest (the first container) but not the containers after it.
  for (const auto& [id, index] : AttachedContainers(elements))
    if (index >= containers.size()) {
      *reason = "the bundle's container " + id + " is missing";
      return false;
    }
  std::set<std::string> listed;
  for (const auto& element : elements)
    if (element.name == "MsiPackage") {
      auto id = Attribute(element, "Id");
      if (!id.empty() && listed.insert(id).second) ids->push_back(std::move(id));
    }
  return true;
}

bool ReadBurnMsiPackage(std::string_view bundle, const std::string& package_id, BurnMsiPackage* package,
                        std::string* reason) {
  *package = BurnMsiPackage{};
  std::vector<std::string_view> found;
  std::vector<Element> elements;
  if (!ReadManifest(bundle, &found, &elements, reason)) return false;
  const auto* containers = &found;
  std::map<std::string, const Element*> payloads;
  const auto attached = AttachedContainers(elements);  // container id → index in containers
  std::vector<std::string> refs;
  bool found_package = false, inside = false;
  for (const auto& element : elements) {
    if (element.name == "Payload") {
      payloads[Attribute(element, "Id")] = &element;
    } else if (element.name == "MsiPackage" && Attribute(element, "Id") == package_id) {
      found_package = true;
      inside = !element.closed;
    } else if (element.name == "/MsiPackage") {
      inside = false;
    } else if (inside && element.name == "PayloadRef") {
      refs.push_back(Attribute(element, "Id"));
    }
  }
  if (!found_package || refs.empty()) {
    *reason = "the bundle has no package " + package_id;
    return false;
  }
  // The package's payloads, by container.
  std::map<std::size_t, std::set<std::string>> wanted;
  std::vector<std::pair<const Element*, std::size_t>> parts;
  std::set<std::string> listed;
  for (const auto& id : refs) {
    if (!listed.insert(id).second) continue;
    const auto payload = payloads.find(id);
    if (payload == payloads.end()) {
      *reason = "the bundle's manifest lists no payload " + id;
      return false;
    }
    const auto container = attached.find(Attribute(*payload->second, "Container"));
    const auto source = Attribute(*payload->second, "SourcePath");
    if (Attribute(*payload->second, "Packaging") != "embedded" || container == attached.end() ||
        container->second >= containers->size() || source.empty()) {
      *reason = "the bundle doesn't carry the payload " + id;
      return false;
    }
    wanted[container->second].insert(source);
    parts.emplace_back(payload->second, container->second);
  }
  std::map<std::size_t, std::map<std::string, std::string>> members;
  for (const auto& [index, names] : wanted)
    if (!Members((*containers)[index], names, &members[index], reason)) return false;
  for (const auto& [payload, index] : parts) {
    auto& data = members[index][Attribute(*payload, "SourcePath")];
    const auto name = FileName(Attribute(*payload, "FilePath"));
    const auto size = Attribute(*payload, "FileSize");
    if (size.empty() || std::strtoull(size.c_str(), nullptr, 10) != data.size()) {
      *reason = "the bundle's copy of " + name + " is not the size its manifest lists";
      return false;
    }
    if (name.empty() || name == "." || name == "..") {
      *reason = "the bundle names a payload Playden can't use";
      return false;
    }
    const auto lower = Lower(name);
    if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".msi") == 0) {
      if (!package->msi_name.empty()) {
        *reason = "the package " + package_id + " has more than one .msi";
        return false;
      }
      package->msi_name = name;
      package->msi = std::move(data);
    } else {
      package->beside[name] = std::move(data);
    }
  }
  if (package->msi_name.empty()) {
    *reason = "the package " + package_id + " has no .msi";
    return false;
  }
  return true;
}

}  // namespace playden::runtime

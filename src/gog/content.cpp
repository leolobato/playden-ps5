// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/content.h"

#include "gog/hash.h"
#include "gog/languages.h"
#include "gog/product_id.h"

#include <algorithm>

namespace playden::gog {
namespace {

using steam::json::Value;

const Value* Member(const Value& object, std::string_view key) { return object.is_object() ? object.Find(key) : nullptr; }

bool Text(const Value& object, std::string_view key, std::string* out) {
  const Value* value = Member(object, key);
  if (!value || !value->is_string()) return false;
  *out = value->text();
  return true;
}

// A JSON number or a string holding a canonical ID (metas send product IDs as strings).
bool Id(const Value* value, std::uint64_t* id) {
  if (!value || !(value->is_number() || value->is_string())) return false;
  return CanonicalId(value->text(), id);
}

// A JSON integer that fits `int`: digits with an optional '-', no fraction or exponent.
bool SmallInt(const Value* value, int* out) {
  if (!value || !value->is_number()) return false;
  std::string_view text = value->text();
  const bool negative = !text.empty() && text.front() == '-';
  if (negative) text.remove_prefix(1);
  if (text.empty() || text.size() > 9) return false;
  int result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    result = result * 10 + (c - '0');
  }
  *out = negative ? -result : result;
  return true;
}

// A size: a non-negative JSON integer up to 1 TiB.
bool Size(const Value* value, std::uint64_t* out) {
  if (!value || !value->is_number()) return false;
  const std::string_view text = value->text();
  if (text.empty() || text.size() > 16 || (text.size() > 1 && text.front() == '0')) return false;
  std::uint64_t result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<std::uint64_t>(c - '0');
  }
  if (result > kMaxContentBytes) return false;
  *out = result;
  return true;
}

bool StringList(const Value* value, std::size_t max_bytes, std::vector<std::string>* out) {
  if (!value || !value->is_array()) return false;
  for (const auto& entry : value->array()) {
    if (!entry.is_string() || entry.text().empty() || entry.text().size() > max_bytes) return false;
    out->push_back(entry.text());
  }
  return true;
}

bool PlainUrlCharacters(std::string_view url) {
  for (const char c : url) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte == 0x7f) return false;
  }
  return true;
}

bool PrintableArguments(std::string_view text) {
  if (text.size() > kMaxDependencyArgumentsBytes) return false;
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte > 0x7e) return false;
  }
  return true;
}

bool HttpsUrl(std::string_view url) { return url.rfind("https://", 0) == 0 && url.size() > 8 && PlainUrlCharacters(url); }

// The last path segment of a URL, without a query.
std::string_view LastSegment(std::string_view url) {
  const auto query = url.find_first_of("?#");
  if (query != std::string_view::npos) url = url.substr(0, query);
  const auto slash = url.rfind('/');
  return slash == std::string_view::npos ? url : url.substr(slash + 1);
}

bool Names(const MetaDepot& depot, const char* bits) {
  return std::find(depot.bitness.begin(), depot.bitness.end(), bits) != depot.bitness.end();
}

// Kept whatever the product has: osBitness absent, or with "*" or "64".
bool AnyBitness(const MetaDepot& depot) { return depot.bitness.empty() || Names(depot, "*") || Names(depot, "64"); }

bool Specific(const std::string& language) { return language != "*"; }

}  // namespace

std::vector<CdnEndpoint> ParseEndpoints(const Value& urls) {
  std::vector<CdnEndpoint> endpoints;
  if (!urls.is_array()) return endpoints;
  for (const auto& entry : urls.array()) {
    if (!entry.is_object()) continue;
    CdnEndpoint endpoint;
    Text(entry, "endpoint_name", &endpoint.name);
    Text(entry, "url_format", &endpoint.url_format);
    Text(entry, "url", &endpoint.url);
    if (endpoint.url_format.empty() && endpoint.url.empty()) continue;
    bool usable = true;
    if (const Value* parameters = Member(entry, "parameters")) {
      if (!parameters->is_object()) continue;
      for (const auto& [key, value] : parameters->object()) {
        if (value.is_string()) {
          endpoint.parameters[key] = value.text();
        } else if (value.is_number() && !value.text().empty() &&
                   value.text().find_first_not_of("0123456789") == std::string::npos) {
          endpoint.parameters[key] = value.text();
        } else {
          usable = false;
          break;
        }
      }
    }
    if (const Value* priority = Member(entry, "priority"); priority && !SmallInt(priority, &endpoint.priority))
      usable = false;
    if (const Value* fallback = Member(entry, "fallback_only")) {
      if (!fallback->is_bool()) usable = false;
      else endpoint.fallback_only = fallback->boolean();
    }
    if (const Value* generations = Member(entry, "supports_generation")) {
      if (!generations->is_array()) usable = false;
      else
        for (const auto& generation : generations->array()) {
          int number = 0;
          if (!SmallInt(&generation, &number)) {
            usable = false;
            break;
          }
          endpoint.generations.push_back(number);
        }
    }
    if (usable) endpoints.push_back(std::move(endpoint));
  }
  return endpoints;
}

std::vector<CdnEndpoint> OrderEndpoints(std::vector<CdnEndpoint> endpoints, int generation) {
  endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(),
                                 [generation](const CdnEndpoint& endpoint) {
                                   return std::find(endpoint.generations.begin(), endpoint.generations.end(),
                                                    generation) == endpoint.generations.end();
                                 }),
                  endpoints.end());
  std::stable_sort(endpoints.begin(), endpoints.end(), [](const CdnEndpoint& left, const CdnEndpoint& right) {
    if (left.fallback_only != right.fallback_only) return !left.fallback_only;
    return left.priority > right.priority;
  });
  return endpoints;
}

std::optional<std::string> FillUrl(const CdnEndpoint& endpoint, std::string_view append_to_path) {
  std::string url;
  if (endpoint.url_format.find('{') == std::string::npos) {
    url = endpoint.url.empty() ? endpoint.url_format : endpoint.url;
    url.append(append_to_path);
  } else {
    auto parameters = endpoint.parameters;
    parameters["path"].append(append_to_path);
    const std::string& format = endpoint.url_format;
    for (std::size_t at = 0; at < format.size();) {
      const auto open = format.find('{', at);
      if (open == std::string::npos) {
        url.append(format, at, std::string::npos);
        break;
      }
      url.append(format, at, open - at);
      const auto close = format.find('}', open + 1);
      if (close == std::string::npos) return std::nullopt;
      const auto found = parameters.find(format.substr(open + 1, close - open - 1));
      if (found == parameters.end()) return std::nullopt;
      url += found->second;
      at = close + 1;
    }
  }
  if (!HttpsUrl(url)) return std::nullopt;
  return url;
}

std::optional<std::string> GalaxyPath(std::string_view hash) {
  if (!IsMd5Hex(hash)) return std::nullopt;
  std::string path;
  path.reserve(hash.size() + 6);
  path.append(hash.substr(0, 2)).append("/").append(hash.substr(2, 2)).append("/").append(hash);
  return path;
}

BuildChoice ChooseBuild(std::string_view body) {
  BuildChoice choice;
  steam::json::Limits limits;
  limits.max_bytes = kMaxBuildsBytes;
  limits.max_values = kMaxBuildsValues;
  limits.max_depth = 16;
  const auto root = steam::json::Parse(body, limits);
  if (!root || !root->is_object()) return choice;
  const Value* items = root->Find("items");
  if (!items || !items->is_array()) return choice;
  const Value* item = nullptr;
  for (const auto& candidate : items->array()) {
    if (!candidate.is_object()) continue;
    const Value* branch = candidate.Find("branch");
    if (branch && !branch->is_null()) continue;
    std::string os;
    if (!Text(candidate, "os", &os) || os != "windows") continue;
    item = &candidate;
    break;
  }
  if (!item) {
    choice.kind = BuildChoice::Kind::kNone;
    return choice;
  }
  std::uint64_t build = 0;
  if (!Id(item->Find("build_id"), &build) || !SmallInt(item->Find("generation"), &choice.generation))
    return choice;
  choice.build_id = item->Find("build_id")->text();
  if (choice.generation != 1 && choice.generation != 2) return choice;
  const Value* urls = item->Find("urls");
  if (!urls || !urls->is_array()) return choice;  // the `link` form: asked without _version=2
  if (choice.generation == 1) {
    std::uint64_t product = 0;
    if (!Id(item->Find("product_id"), &product) || !Id(item->Find("legacy_build_id"), &choice.timestamp))
      return choice;
    auto endpoints = OrderEndpoints(ParseEndpoints(*urls), 1);
    if (endpoints.empty()) return choice;
    // Unlike a gen 2 meta, every endpoint must name this product's repository at this timestamp: one that names
    // another is not a mirror of it.
    const std::string suffix = "/content-system/v1/manifests/" + std::to_string(product) + "/windows/" +
                               std::to_string(choice.timestamp) + "/repository.json";
    for (const auto& endpoint : endpoints) {
      const auto url = FillUrl(endpoint, "");
      if (!url || url->size() <= suffix.size() || url->compare(url->size() - suffix.size(), suffix.size(), suffix) != 0)
        return choice;
    }
    choice.repository = std::move(endpoints);
    choice.kind = BuildChoice::Kind::kGen1;
    return choice;
  }
  auto endpoints = OrderEndpoints(ParseEndpoints(*urls), 2);
  if (endpoints.empty()) return choice;
  const auto& first = endpoints.front();
  const auto meta_id = LastSegment(first.url.empty() ? first.url_format : first.url);
  if (!IsMd5Hex(meta_id)) return choice;
  choice.meta_id = std::string(meta_id);
  // Every endpoint must point at the same meta.
  endpoints.erase(std::remove_if(endpoints.begin(), endpoints.end(),
                                 [&](const CdnEndpoint& endpoint) {
                                   return LastSegment(endpoint.url.empty() ? endpoint.url_format : endpoint.url) !=
                                          choice.meta_id;
                                 }),
                  endpoints.end());
  choice.meta = std::move(endpoints);
  choice.kind = BuildChoice::Kind::kGen2;
  return choice;
}

std::optional<Meta> ParseMeta(std::string_view inflated) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxMetaBytes;
  limits.max_values = kMaxMetaValues;
  limits.max_depth = 16;
  const auto root = steam::json::Parse(inflated, limits);
  if (!root || !root->is_object()) return std::nullopt;
  if (const Value* version = root->Find("version")) {
    int number = 0;
    if (!SmallInt(version, &number) || number != 2) return std::nullopt;
  }
  Meta meta;
  if (!Id(root->Find("baseProductId"), &meta.base_product)) return std::nullopt;
  std::uint64_t build = 0;
  if (!Id(root->Find("buildId"), &build)) return std::nullopt;
  meta.build_id = root->Find("buildId")->text();
  if (const Value* directory = root->Find("installDirectory")) {
    if (!directory->is_string()) return std::nullopt;
    meta.install_directory = directory->text();
  }
  if (const Value* dependencies = root->Find("dependencies")) {
    if (!StringList(dependencies, 64, &meta.dependencies)) return std::nullopt;
    for (const auto& id : meta.dependencies)
      if (!ValidDependencyId(id)) return std::nullopt;
  }
  if (const Value* products = root->Find("products")) {
    if (!products->is_array()) return std::nullopt;
    for (const auto& product : products->array()) {
      std::uint64_t id = 0;
      if (!product.is_object() || !Id(product.Find("productId"), &id)) return std::nullopt;
      std::string name;
      if (const Value* value = product.Find("name"); value && value->is_string()) name = value->text();
      meta.products.emplace_back(id, std::move(name));
    }
  }
  const Value* depots = root->Find("depots");
  if (!depots || !depots->is_array()) return std::nullopt;
  for (const auto& entry : depots->array()) {
    if (!entry.is_object()) return std::nullopt;
    MetaDepot depot;
    if (!Id(entry.Find("productId"), &depot.product)) return std::nullopt;
    if (!StringList(entry.Find("languages"), 64, &depot.languages) || depot.languages.empty()) return std::nullopt;
    if (!Text(entry, "manifest", &depot.manifest) || !IsMd5Hex(depot.manifest)) return std::nullopt;
    if (!Size(entry.Find("size"), &depot.size) || !Size(entry.Find("compressedSize"), &depot.compressed_size))
      return std::nullopt;
    if (const Value* bitness = entry.Find("osBitness"); bitness && !StringList(bitness, 8, &depot.bitness))
      return std::nullopt;
    if (const Value* gog = entry.Find("isGogDepot")) {
      if (!gog->is_bool()) return std::nullopt;
      depot.gog_depot = gog->boolean();
    }
    meta.depots.push_back(std::move(depot));
  }
  return meta;
}

bool PlainLanguageCode(std::string_view code) {
  if (code.empty() || code.size() > 16) return false;
  for (const char c : code)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return false;
  return true;
}

Selection SelectDepots(const Meta& meta, const std::set<std::uint64_t>& owned, std::string_view steam_code) {
  Selection selection;
  selection.products.push_back(meta.base_product);
  for (const auto& [id, name] : meta.products) {
    (void)name;
    if (id == meta.base_product || !owned.count(id)) continue;
    if (std::find(selection.products.begin(), selection.products.end(), id) == selection.products.end())
      selection.products.push_back(id);
  }

  // The base depots that decide the language: the non-gog ones that name one, else every base depot.
  std::vector<const MetaDepot*> carriers;
  for (const auto& depot : meta.depots)
    if (depot.product == meta.base_product && !depot.gog_depot &&
        std::any_of(depot.languages.begin(), depot.languages.end(), Specific))
      carriers.push_back(&depot);
  if (carriers.empty())
    for (const auto& depot : meta.depots)
      if (depot.product == meta.base_product) carriers.push_back(&depot);
  const auto carried = [&carriers](const std::vector<std::string_view>& codes) {
    if (codes.empty()) return false;
    return std::any_of(carriers.begin(), carriers.end(),
                       [&codes](const MetaDepot* depot) { return CarriesAny(depot->languages, codes); });
  };

  std::vector<std::string_view> codes = LanguageCodes(steam_code);
  std::string fallback;  // a GOG code with no Steam name, kept alive for `codes`
  if (!carried(codes)) {
    codes = LanguageCodes("english");
    if (!carried(codes)) {
      codes.clear();
      // A code the plan can't hold (a tab, 40 bytes) is passed over: it would be recorded and never read back. A name
      // the table knows ("Brazilian Portuguese") stands, as its plain code is what is recorded.
      const auto usable = [](const std::string& code) {
        return Specific(code) && (!SteamCodeFor(code).empty() || PlainLanguageCode(code));
      };
      for (const MetaDepot* depot : carriers) {
        const auto first = std::find_if(depot->languages.begin(), depot->languages.end(), usable);
        if (first == depot->languages.end()) continue;
        const auto steam = SteamCodeFor(*first);
        if (!steam.empty()) {
          codes = LanguageCodes(steam);
        } else {
          fallback = *first;
          codes = {fallback};
        }
        break;
      }
    }
  }
  if (codes.empty()) {
    // Every base depot is "*": nothing to choose by. Record the requested language, or English.
    const auto requested = GogCode(steam_code);
    selection.language = requested.empty() ? GogCode("english") : requested;
  } else {
    selection.language = std::string(codes.front());
  }

  const auto keeps_language = [&codes](const MetaDepot& depot) {
    return std::find(depot.languages.begin(), depot.languages.end(), "*") != depot.languages.end() ||
           CarriesAny(depot.languages, codes);
  };
  for (const auto product : selection.products) {
    std::vector<const MetaDepot*> chosen;
    for (const auto& depot : meta.depots)
      if (depot.product == product && keeps_language(depot)) chosen.push_back(&depot);
    // A 32-bit depot goes only when the product has a depot that names 64: an untagged gog depot next to a 32-bit
    // game depot must not leave the game out.
    const bool has64 = std::any_of(chosen.begin(), chosen.end(), [](const MetaDepot* depot) { return Names(*depot, "64"); });
    for (const MetaDepot* depot : chosen) {
      if (has64 && !AnyBitness(*depot)) continue;
      selection.depots.push_back(*depot);
      selection.size += depot->size;
      selection.compressed_size += depot->compressed_size;
    }
  }
  return selection;
}

std::optional<DependencyRepository> ParseDependencyRepository(std::string_view body) {
  steam::json::Limits limits;
  limits.max_bytes = 64 * 1024;
  limits.max_values = 256;
  limits.max_depth = 4;
  const auto root = steam::json::Parse(body, limits);
  if (!root || !root->is_object()) return std::nullopt;
  DependencyRepository repository;
  if (!Text(*root, "repository_manifest", &repository.manifest_url) || !HttpsUrl(repository.manifest_url))
    return std::nullopt;
  std::uint64_t build = 0;
  if (!Id(root->Find("build_id"), &build)) return std::nullopt;
  repository.build_id = root->Find("build_id")->text();
  if (!SmallInt(root->Find("generation"), &repository.generation)) return std::nullopt;
  return repository;
}

std::optional<std::vector<Dependency>> ParseDependencyManifest(std::string_view inflated) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxDependencyManifestBytes;
  limits.max_values = kMaxDependencyManifestValues;
  limits.max_depth = 8;
  const auto root = steam::json::Parse(inflated, limits);
  if (!root || !root->is_object()) return std::nullopt;
  const Value* depots = root->Find("depots");
  if (!depots || !depots->is_array()) return std::nullopt;
  std::vector<Dependency> dependencies;
  for (const auto& entry : depots->array()) {
    Dependency dependency;
    if (!entry.is_object() || !Text(entry, "dependencyId", &dependency.id) || !ValidDependencyId(dependency.id))
      continue;
    if (!Text(entry, "manifest", &dependency.manifest) || !IsMd5Hex(dependency.manifest)) continue;
    if (!Size(entry.Find("size"), &dependency.size) || !Size(entry.Find("compressedSize"), &dependency.compressed_size))
      continue;
    if (const Value* executable = entry.Find("executable")) {
      if (!executable->is_object()) continue;
      if (const Value* path = executable->Find("path")) {
        if (!path->is_string()) continue;
        dependency.exe_path = path->text();
      }
      if (const Value* arguments = executable->Find("arguments")) {
        if (arguments->is_string() && PrintableArguments(arguments->text())) {
          dependency.arguments = arguments->text();
        } else {
          dependency.usable_arguments = false;
        }
      }
    }
    if (!StringList(entry.Find("osBitness"), 8, &dependency.bitness)) dependency.bitness.clear();
    dependencies.push_back(std::move(dependency));
  }
  return dependencies;
}

bool GameFolderDependency(const Dependency& dependency) {
  static constexpr std::string_view kRedist = "__redist";
  if (dependency.exe_path.size() < kRedist.size()) return true;
  for (std::size_t i = 0; i < kRedist.size(); ++i) {
    char c = dependency.exe_path[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kRedist[i]) return true;
  }
  return false;
}

bool ValidDependencyId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (const char c : id)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
          c == '-'))
      return false;
  return true;
}

}  // namespace playden::gog

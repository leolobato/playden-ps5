// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/manifest_v1.h"

#include "gog/content.h"
#include "gog/hash.h"
#include "gog/languages.h"
#include "gog/product_id.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <utility>

namespace playden::gog {
namespace {

using steam::json::Value;

bool EqualFold(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    char a = left[i], b = right[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// A JSON number or a string holding a canonical ID.
bool Id(const Value* value, std::uint64_t* id) {
  if (!value || !(value->is_number() || value->is_string())) return false;
  return CanonicalId(value->text(), id);
}

// A JSON integer or a string of decimal digits ("0" or no leading zero) up to `max`.
bool Decimal(const Value* value, std::uint64_t max, std::uint64_t* out) {
  if (!value || !(value->is_number() || value->is_string())) return false;
  const std::string& text = value->text();
  if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) return false;
  std::uint64_t result = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const auto digit = static_cast<std::uint64_t>(c - '0');
    if (result > (max - digit) / 10) return false;
    result = result * 10 + digit;
  }
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

// An optional string member: absent leaves `out` empty; any other type refuses the entry.
bool OptionalText(const Value& object, std::string_view key, std::size_t max_bytes, std::string* out) {
  const Value* value = object.Find(key);
  if (!value) return true;
  if (!value->is_string() || value->text().size() > max_bytes) return false;
  *out = value->text();
  return true;
}

bool Neutral(const std::string& language) { return language == "*" || EqualFold(language, "Neutral"); }

bool Windows(const RepositoryDepotV1& depot) {
  return depot.systems.empty() || std::any_of(depot.systems.begin(), depot.systems.end(), [](const std::string& system) {
           return EqualFold(system, "Windows");
         });
}

bool Names(const RepositoryDepotV1& depot, std::uint64_t product) {
  return std::find(depot.products.begin(), depot.products.end(), product) != depot.products.end();
}

enum class DepotEntry { kFile, kRedist, kSkipped, kBadManifest };

DepotEntry ParseDepot(const Value& entry, RepositoryDepotV1* depot, RedistV1* redist) {
  if (!entry.is_object()) return DepotEntry::kSkipped;
  if (const Value* id = entry.Find("redist")) {
    if (!id->is_string() || !ValidDependencyId(id->text())) return DepotEntry::kSkipped;
    redist->id = id->text();
    if (!OptionalText(entry, "executable", 1024, &redist->executable) ||
        !OptionalText(entry, "argument", 1024, &redist->argument) ||
        !OptionalText(entry, "targetDir", 1024, &redist->target_dir))
      return DepotEntry::kSkipped;
    return DepotEntry::kRedist;
  }
  if (!StringList(entry.Find("languages"), 64, &depot->languages) || depot->languages.empty())
    return DepotEntry::kSkipped;
  const Value* products = entry.Find("gameIDs");
  if (!products || !products->is_array() || products->array().empty()) return DepotEntry::kSkipped;
  for (const auto& product : products->array()) {
    std::uint64_t id = 0;
    if (!Id(&product, &id)) return DepotEntry::kSkipped;
    depot->products.push_back(id);
  }
  if (const Value* systems = entry.Find("systems"); systems && !StringList(systems, 64, &depot->systems))
    return DepotEntry::kSkipped;
  if (!Decimal(entry.Find("size"), kMaxContentBytes, &depot->size)) return DepotEntry::kSkipped;
  const Value* manifest = entry.Find("manifest");
  if (!manifest || !manifest->is_string() || !ValidManifestNameV1(manifest->text())) return DepotEntry::kBadManifest;
  depot->manifest = manifest->text();
  return DepotEntry::kFile;
}

}  // namespace

bool ValidManifestNameV1(std::string_view name) {
  constexpr std::string_view kSuffix = ".json";
  if (name.size() <= kSuffix.size() || name.size() > 64 + kSuffix.size()) return false;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
  for (const char c : name.substr(0, name.size() - kSuffix.size()))
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

std::optional<RepositoryV1> ParseRepositoryV1(std::string_view body) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxRepositoryV1Bytes;
  limits.max_values = kMaxRepositoryV1Values;
  limits.max_depth = kMaxRepositoryV1Depth;
  const auto root = steam::json::Parse(body, limits);
  if (!root || !root->is_object()) return std::nullopt;
  std::uint64_t version = 0;
  if (!Decimal(root->Find("version"), 1000, &version) || version != 1 || !root->Find("version")->is_number())
    return std::nullopt;
  const Value* product = root->Find("product");
  if (!product || !product->is_object()) return std::nullopt;

  RepositoryV1 repository;
  if (!Id(product->Find("rootGameID"), &repository.root_product) ||
      !Id(product->Find("timestamp"), &repository.timestamp))
    return std::nullopt;
  if (const Value* directory = product->Find("installDirectory")) {
    if (!directory->is_string()) return std::nullopt;
    repository.install_directory = directory->text();
  }
  if (const Value* products = product->Find("gameIDs")) {
    if (!products->is_array()) return std::nullopt;
    for (const auto& entry : products->array()) {
      std::uint64_t id = 0;
      if (!entry.is_object() || !Id(entry.Find("gameID"), &id)) {
        ++repository.skipped;
        continue;
      }
      if (std::find(repository.products.begin(), repository.products.end(), id) == repository.products.end())
        repository.products.push_back(id);
    }
  }
  const Value* depots = product->Find("depots");
  if (!depots || !depots->is_array()) return std::nullopt;
  for (const auto& entry : depots->array()) {
    RepositoryDepotV1 depot;
    RedistV1 redist;
    switch (ParseDepot(entry, &depot, &redist)) {
      case DepotEntry::kFile: repository.depots.push_back(std::move(depot)); break;
      case DepotEntry::kRedist: repository.redists.push_back(std::move(redist)); break;
      case DepotEntry::kSkipped: ++repository.skipped; break;
      case DepotEntry::kBadManifest: ++repository.bad_manifests; break;
    }
  }
  if (const Value* commands = product->Find("support_commands")) {
    if (!commands->is_array()) return std::nullopt;
    repository.support_commands = commands->array().size();
  }
  return repository;
}

SelectionV1 SelectDepotsV1(const RepositoryV1& repository, const std::set<std::uint64_t>& owned,
                           std::string_view steam_code) {
  SelectionV1 selection;
  selection.products.push_back(repository.root_product);
  for (const auto id : repository.products)
    if (id != repository.root_product && owned.count(id) &&
        std::find(selection.products.begin(), selection.products.end(), id) == selection.products.end())
      selection.products.push_back(id);

  // The root's Windows depots decide the language; a Neutral one carries none in particular.
  std::vector<const RepositoryDepotV1*> carriers;
  for (const auto& depot : repository.depots)
    if (Names(depot, repository.root_product) && Windows(depot)) carriers.push_back(&depot);
  const auto carried = [&carriers](const std::vector<std::string_view>& codes) {
    if (codes.empty()) return false;
    return std::any_of(carriers.begin(), carriers.end(),
                       [&codes](const RepositoryDepotV1* depot) { return CarriesAny(depot->languages, codes); });
  };

  std::vector<std::string_view> codes = LanguageCodes(steam_code);
  std::string fallback;  // a name with no Steam language, kept alive for `codes`
  if (!carried(codes)) {
    codes = LanguageCodes("english");
    if (!carried(codes)) {
      codes.clear();
      const auto usable = [](const std::string& language) {
        return !Neutral(language) && (!SteamCodeFor(language).empty() || PlainLanguageCode(language));
      };
      for (const RepositoryDepotV1* depot : carriers) {
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
    const auto requested = GogCode(steam_code);
    selection.language = requested.empty() ? GogCode("english") : requested;
  } else {
    selection.language = std::string(codes.front());
  }

  for (const auto& depot : repository.depots) {
    // gameIDs order: a depot naming an unowned DLC first and the base game after belongs to the base game.
    const auto product = std::find_if(depot.products.begin(), depot.products.end(), [&selection](std::uint64_t id) {
      return std::find(selection.products.begin(), selection.products.end(), id) != selection.products.end();
    });
    if (product == depot.products.end() || !Windows(depot)) continue;
    if (!std::any_of(depot.languages.begin(), depot.languages.end(), Neutral) && !CarriesAny(depot.languages, codes))
      continue;
    selection.depots.push_back(depot);
    selection.depots.back().selected_product = *product;
    selection.size += depot.size;
  }
  selection.compressed_size = selection.size;
  for (const auto& redist : repository.redists)
    if (std::find(selection.dependencies.begin(), selection.dependencies.end(), redist.id) ==
        selection.dependencies.end())
      selection.dependencies.push_back(redist.id);
  return selection;
}

bool ParseDepotItemV1(const Value& value, DepotItem* item) {
  if (!value.is_object()) return false;
  const Value* path = value.Find("path");
  if (!path || !path->is_string()) return false;
  item->path = path->text();
  if (const Value* support = value.Find("support")) {
    if (!support->is_bool()) return false;
    item->support = support->boolean();
  }
  if (const Value* executable = value.Find("executable")) {
    if (!executable->is_bool()) return false;
    item->executable = executable->boolean();  // no meaning on the console
  }
  if (const Value* directory = value.Find("directory")) {
    if (!directory->is_bool()) return false;
    if (directory->boolean()) {
      item->type = DepotItem::Type::kDirectory;
      return true;
    }
  }
  const Value* size = value.Find("size");
  if (value.Find("symlinkType") || value.Find("target") || !size) {
    item->type = DepotItem::Type::kLink;
    return true;
  }
  item->type = DepotItem::Type::kFile;
  const Value* hash = value.Find("hash");
  if (!hash || !hash->is_string() || !IsMd5Hex(hash->text())) return false;
  item->md5 = hash->text();
  if (!Decimal(size, kMaxFileBytes, &item->size) || !Decimal(value.Find("offset"), UINT64_MAX, &item->offset))
    return false;
  if (item->size > UINT64_MAX - item->offset) return false;
  if (const Value* url = value.Find("url")) {
    if (!url->is_string()) return false;
    const std::string_view text = url->text();
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || slash + 1 >= text.size() ||
        !CanonicalId(text.substr(0, slash), &item->blob_product))
      return false;
  }
  return true;
}

DepotItemReader DepotItemReaderV1(DepotItemReader::Callback callback) {
  return DepotItemReader(std::move(callback), "files", &ParseDepotItemV1);
}

}  // namespace playden::gog

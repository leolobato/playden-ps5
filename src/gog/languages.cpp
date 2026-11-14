// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/languages.h"

#include <utility>

namespace playden::gog {
namespace {

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

using Entry = std::pair<std::string_view, std::vector<std::string_view>>;

// The codes are gogdl's (GOG_PROTOCOL.md, depot selection) and the Mac app's table. A two-letter code is listed only
// where it is not another language's: "es" is Spain's Spanish, not Latin America's, and "zh" is Simplified Chinese.
// Steam's Arabic, Bulgarian, Thai and Vietnamese have no entry: a game in one of them installs in English.
const std::vector<Entry>& Table() {
  static const std::vector<Entry> table = {
      {"english", {"en-US", "en", "English"}},
      {"german", {"de-DE", "de", "German"}},
      {"french", {"fr-FR", "fr", "French"}},
      {"italian", {"it-IT", "it", "Italian"}},
      {"spanish", {"es-ES", "es", "Spanish"}},
      {"latam", {"es-MX", "Latin American Spanish"}},
      {"brazilian", {"pt-BR", "br", "Brazilian Portuguese"}},
      {"portuguese", {"pt-PT", "pt", "Portuguese"}},
      {"russian", {"ru-RU", "ru", "Russian"}},
      {"polish", {"pl-PL", "pl", "Polish"}},
      {"japanese", {"ja-JP", "jp", "Japanese"}},
      {"koreana", {"ko-KR", "ko", "Korean"}},
      {"schinese", {"zh-Hans", "cn", "zh", "Chinese Simplified"}},
      {"tchinese", {"zh-Hant", "Chinese Traditional"}},
      {"czech", {"cs-CZ", "cz", "Czech"}},
      {"dutch", {"nl-NL", "nl", "Dutch"}},
      {"turkish", {"tr-TR", "tr", "Turkish"}},
      {"hungarian", {"hu-HU", "hu", "Hungarian"}},
      {"swedish", {"sv-SE", "sv", "Swedish"}},
      {"danish", {"da-DK", "da", "Danish"}},
      {"finnish", {"fi-FI", "fi", "Finnish"}},
      {"norwegian", {"nb-NO", "nb", "Norwegian"}},
      {"ukrainian", {"uk-UA", "uk", "Ukrainian"}},
      {"greek", {"el-GR", "el", "Greek"}},
      {"romanian", {"ro-RO", "ro", "Romanian"}},
  };
  return table;
}

}  // namespace

const std::vector<std::string_view>& LanguageCodes(std::string_view steam_code) {
  static const std::vector<std::string_view> none;
  for (const auto& [steam, codes] : Table())
    if (EqualFold(steam, steam_code)) return codes;
  return none;
}

bool CarriesAny(const std::vector<std::string>& depot_languages, const std::vector<std::string_view>& codes) {
  for (const auto& language : depot_languages)
    for (const auto code : codes)
      if (EqualFold(language, code)) return true;
  return false;
}

bool LanguageMatches(const std::vector<std::string>& depot_languages, std::string_view steam_code) {
  for (const auto& language : depot_languages)
    if (language == "*") return true;
  return CarriesAny(depot_languages, LanguageCodes(steam_code));
}

std::string GogCode(std::string_view steam_code) {
  const auto& codes = LanguageCodes(steam_code);
  return codes.empty() ? std::string() : std::string(codes.front());
}

std::string SteamCodeFor(std::string_view gog_code) {
  for (const auto& [steam, codes] : Table())
    for (const auto code : codes)
      if (EqualFold(code, gog_code)) return std::string(steam);
  return {};
}

}  // namespace playden::gog

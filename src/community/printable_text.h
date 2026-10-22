#pragma once
// printable_text.h - The one rule for text a player types for a shared recipe (its title, their
// name): what the Playden API and the recipes checker accept, roughly Python's str.isprintable.
// Header-only so the preferences (which keep the name) use it without linking the community code.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace playden::community {

// Valid UTF-8 of min-max characters with no control, format (zero-width, bidi marks and
// isolates, soft hyphen, BOM), separator other than the plain space (U+2000-type and U+3000 spaces,
// line and paragraph separators), surrogate, private-use or noncharacter-block code points.
inline bool IsSubmissionText(std::string_view text, std::size_t min_characters, std::size_t max_characters) {
  std::size_t characters = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::uint32_t code = 0;
    std::size_t length = 0;
    if (lead < 0x80) code = lead, length = 1;
    else if ((lead & 0xe0) == 0xc0) code = lead & 0x1f, length = 2;
    else if ((lead & 0xf0) == 0xe0) code = lead & 0x0f, length = 3;
    else if ((lead & 0xf8) == 0xf0) code = lead & 0x07, length = 4;
    else return false;
    if (i + length > text.size()) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 0x3f);
    }
    // Overlong forms and values past Unicode.
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
        code > 0x10ffff)
      return false;
    if (code < 0x20 || (code >= 0x7f && code <= 0xa0) || code == 0xad || code == 0x1680 ||
        (code >= 0x2000 && code <= 0x200f) || (code >= 0x2028 && code <= 0x202f) || (code >= 0x205f && code <= 0x206f) ||
        code == 0x3000 || (code >= 0xd800 && code <= 0xdfff) || (code >= 0xe000 && code <= 0xf8ff) || code == 0xfeff ||
        (code >= 0xfff0 && code <= 0xfffb) || code >= 0xf0000 || code == 0x061c || code == 0x180e)
      return false;
    i += length;
    if (++characters > max_characters) return false;
  }
  return characters >= min_characters;
}

}  // namespace playden::community

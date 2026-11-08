// system_keyboard.cpp - The PS5's own keyboard (the IME dialog) for Playden's text fields.
// Adapted from ProsperoStore's platform/ps5/ime (BlackBearReloaded, GPL-3.0-or-later).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/ps5/system_keyboard.h"

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <time.h>

namespace {
struct ImeParam {
  std::int32_t user_id, type;
  std::uint64_t supported_languages;
  std::int32_t enter_label, input_method;
  void* filter;
  std::uint32_t option, max_text_length;
  std::uint16_t* input_text_buffer;
  float pos_x, pos_y;
  std::int32_t horizontal_alignment, vertical_alignment;
  const std::uint16_t *placeholder, *title;
  std::int8_t reserved[16];
};
struct ImeResult {
  std::int32_t outcome;  // 0 accepted, 1 cancelled, 2 aborted
  std::int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96);
static_assert(offsetof(ImeParam, input_text_buffer) == 40);
static_assert(offsetof(ImeParam, title) == 72);
static_assert(sizeof(ImeResult) == 16);

constexpr std::uint16_t kImeDialogModule = 0x0096;
constexpr std::uint32_t kOptionPassword = 0x4;
constexpr std::uint32_t kAlreadyInitialized = 0x80b80002u;
}  // namespace

// The payload SDK has no libSceCommonDialog stub: the build links a link-only one (cmake/HomebrewPS5.cmake).
extern "C" {
int sceCommonDialogInitialize();
int sceImeDialogAbort();
int sceImeDialogGetResult(ImeResult* result);
int sceImeDialogGetStatus();
int sceImeDialogInit(const ImeParam* param, const void* extended);
int sceImeDialogTerm();
int sceSysmoduleLoadModule(std::uint16_t id);
int sceUserServiceGetForegroundUser(std::int32_t* user_id);
}

namespace playden::ps5 {
namespace {
std::int64_t MonotonicUs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

// UTF-8 into a zero-terminated UTF-16 buffer, cut at a whole character when it does not fit.
void ToUtf16(std::string_view source, std::span<std::uint16_t> out) {
  std::size_t written = 0, index = 0;
  while (index < source.size() && written + 1 < out.size()) {
    const auto lead = static_cast<unsigned char>(source[index]);
    const unsigned length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
    std::uint32_t point = 0xfffd;
    if (length == 0 || index + length > source.size()) {
      ++index;
    } else {
      point = length == 1 ? lead : lead & (0x7f >> length);
      for (unsigned i = 1; i < length; ++i) point = (point << 6) | (static_cast<unsigned char>(source[index + i]) & 0x3f);
      index += length;
    }
    if (point >= 0x10000) {
      if (written + 2 >= out.size()) break;
      point -= 0x10000;
      out[written++] = static_cast<std::uint16_t>(0xd800 | (point >> 10));
      out[written++] = static_cast<std::uint16_t>(0xdc00 | (point & 0x3ff));
    } else {
      out[written++] = static_cast<std::uint16_t>(point);
    }
  }
  out[written] = 0;
}

std::string ToUtf8(std::span<const std::uint16_t> source) {
  std::string out;
  for (std::size_t i = 0; i < source.size() && source[i] != 0; ++i) {
    std::uint32_t point = source[i];
    if (point >= 0xd800 && point <= 0xdbff && i + 1 < source.size() && source[i + 1] >= 0xdc00 &&
        source[i + 1] <= 0xdfff)
      point = 0x10000 + ((point - 0xd800) << 10) + (source[++i] - 0xdc00);
    else if (point >= 0xd800 && point <= 0xdfff)
      point = 0xfffd;
    if (point < 0x80) {
      out.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      out.push_back(static_cast<char>(0xc0 | (point >> 6)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else if (point < 0x10000) {
      out.push_back(static_cast<char>(0xe0 | (point >> 12)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (point >> 18)));
      out.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (point & 0x3f)));
    }
  }
  return out;
}

// Set by Preload: the dialog is initialized and its module loaded.
bool g_loaded = false;
int g_preload_result = 0;

std::string Refused(const char* step, int result) {
  char text[64];
  std::snprintf(text, sizeof(text), "%s=0x%08x", step, static_cast<unsigned>(result));
  return text;
}
}  // namespace

SystemKeyboard::~SystemKeyboard() {
  Close();
  Wipe();
}

int SystemKeyboard::Preload() {
  if (g_loaded) return 0;
  const int initialized = sceCommonDialogInitialize();
  if (initialized < 0 && static_cast<std::uint32_t>(initialized) != kAlreadyInitialized) return g_preload_result = initialized;
  if (const int loaded = sceSysmoduleLoadModule(kImeDialogModule); loaded < 0) return g_preload_result = loaded;
  g_loaded = true;
  return g_preload_result = 0;
}

void SystemKeyboard::Wipe() {
  volatile std::uint16_t* units = buffer_.data();
  for (std::size_t i = 0; i < buffer_.size(); ++i) units[i] = 0;
  volatile char* bytes = text_.data();
  for (std::size_t i = 0; i < text_.size(); ++i) bytes[i] = 0;
  text_.clear();
}

bool SystemKeyboard::Open(std::string_view title, std::string_view value, bool secret, std::string* error) {
  if (active_) return false;
  if (!g_loaded) {
    if (error) *error = Refused("preload", g_preload_result);
    return false;
  }
  ImeParam param{};
  if (const int user = sceUserServiceGetForegroundUser(&param.user_id); user < 0) {
    if (error) *error = Refused("user", user);
    return false;
  }
  // A value longer than the dialog holds would come back cut: Playden's own keyboard edits it.
  std::size_t units = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto byte = static_cast<unsigned char>(value[i]);
    if ((byte & 0xc0) != 0x80) units += byte >= 0xf0 ? 2 : 1;
  }
  if (units > (secret ? 64u : 2048u)) {
    if (error) *error = "value-too-long units=" + std::to_string(units);
    return false;
  }
  Wipe();
  ToUtf16(title, title_);
  // A password dialog takes only the basic Latin type (1); the default type is refused with
  // 0x80bc0030 (console, 2026-10-17). Settings are tried in turn and every refusal is logged.
  struct Attempt { std::int32_t type; std::uint32_t max_length; bool placeholder; };
  // 2048 for long launch arguments; 511 was all the dialog was asked for before, so it stays the fallback for a
  // value that fits.
  static constexpr Attempt kPlain[] = {{0, 2048, true}, {0, 511, true}};
  static constexpr Attempt kSecret[] = {{1, 128, true}, {1, 64, false}};
  std::string refusals;
  bool opened = false;
  for (const auto& attempt : secret ? std::span<const Attempt>(kSecret) : std::span<const Attempt>(kPlain)) {
    if (units > attempt.max_length) continue;  // it would come back cut
    ToUtf16(value, std::span<std::uint16_t>(buffer_.data(), attempt.max_length + 1));
    param.type = attempt.type;
    param.option = secret ? kOptionPassword : 0;
    param.max_text_length = attempt.max_length;
    param.input_text_buffer = buffer_.data();
    param.pos_x = 960.0F;  // the middle of the 1920x1080 screen
    param.pos_y = 540.0F;
    param.horizontal_alignment = param.vertical_alignment = 1;  // centred on that point
    param.placeholder = attempt.placeholder ? title_.data() : nullptr;
    param.title = title_.data();
    const int result = sceImeDialogInit(&param, nullptr);
    if (result == 0) { opened = true; break; }
    char refusal[80];
    std::snprintf(refusal, sizeof(refusal), "%sinit(type=%d max=%u placeholder=%d)=0x%08x", refusals.empty() ? "" : " ",
                  int(attempt.type), unsigned(attempt.max_length), attempt.placeholder ? 1 : 0, unsigned(result));
    refusals += refusal;
  }
  if (error) *error = refusals;
  if (!opened) {
    Wipe();
    return false;
  }
  active_ = true;
  started_us_ = MonotonicUs();
  return true;
}

SystemKeyboard::State SystemKeyboard::Poll() {
  if (!active_) return State::kIdle;
  // The status can read "none" for a moment after Init.
  const int status = sceImeDialogGetStatus();
  if (status == 1 || (status == 0 && MonotonicUs() - started_us_ < 1000000)) return State::kOpen;
  State state = State::kFailed;
  if (status == 2) {
    ImeResult result{};
    if (sceImeDialogGetResult(&result) >= 0) {
      state = result.outcome == 0 ? State::kAccepted : State::kCancelled;
      if (state == State::kAccepted) text_ = ToUtf8(buffer_);
    }
  }
  volatile std::uint16_t* units = buffer_.data();
  for (std::size_t i = 0; i < buffer_.size(); ++i) units[i] = 0;
  // Focus stays with the system while it still owns the dialog.
  if (sceImeDialogTerm() < 0) return State::kOpen;
  active_ = false;
  return state;
}

void SystemKeyboard::Close() {
  if (!active_) return;
  sceImeDialogAbort();
  active_ = sceImeDialogTerm() < 0;
  if (!active_) Wipe();
}

std::string SystemKeyboard::Take() {
  std::string text = std::move(text_);
  Wipe();
  return text;
}
}  // namespace playden::ps5

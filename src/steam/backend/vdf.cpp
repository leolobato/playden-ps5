#include "steam/backend/vdf.h"

#include <charconv>

namespace playden::steam {
namespace {

bool EqualsAsciiNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

enum class Status { kOk, kNone, kError };

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Status ParseTop(VdfNode* root) {
    root->is_dict = true;
    Status status = ParseEntries(root, 0);
    if (status == Status::kError) return status;
    SkipWhitespaceAndComments();
    // A stray closing brace at the top level is malformed.
    return pos_ == text_.size() ? Status::kOk : Status::kError;
  }

 private:
  static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  void SkipWhitespaceAndComments() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (IsSpace(c)) {
        ++pos_;
        continue;
      }
      if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        continue;
      }
      break;
    }
  }

  // kOk: token produced; kNone: end of input; kError: unterminated string. `structural`: the token is an unquoted
  // brace. A quoted "{" or "}" is a string like any other.
  Status ParseToken(std::string* out, bool* structural) {
    out->clear();
    *structural = false;
    SkipWhitespaceAndComments();
    if (pos_ >= text_.size()) return Status::kNone;
    if (text_[pos_] == '"') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] != '"') {
        if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
          ++pos_;
          switch (text_[pos_]) {
            case 'n': out->push_back('\n'); break;
            case 't': out->push_back('\t'); break;
            case '\\': out->push_back('\\'); break;
            case '"': out->push_back('"'); break;
            default:
              out->push_back('\\');
              out->push_back(text_[pos_]);
              break;
          }
        } else {
          out->push_back(text_[pos_]);
        }
        ++pos_;
        if (out->size() > kMaxVdfBytes) return Status::kError;
      }
      if (pos_ >= text_.size()) return Status::kError;
      ++pos_;
      return Status::kOk;
    }
    if (text_[pos_] == '{' || text_[pos_] == '}') {
      out->push_back(text_[pos_]);
      *structural = true;
      ++pos_;
      return Status::kOk;
    }
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (IsSpace(c) || c == '{' || c == '}' || c == '"') break;
      out->push_back(c);
      ++pos_;
    }
    return out->empty() ? Status::kNone : Status::kOk;
  }

  // Parses one `key value` or `key { ... }` pair into parent. kNone at end or closing brace.
  Status ParsePair(VdfNode* parent, std::size_t depth) {
    const std::size_t save = pos_;
    std::string key;
    bool key_structural = false;
    Status status = ParseToken(&key, &key_structural);
    if (status == Status::kError) return status;
    if (status == Status::kNone) return Status::kNone;
    if (key_structural && key == "}") {
      pos_ = save;
      return Status::kNone;
    }
    if (key_structural) return Status::kError;  // a dict without a key
    std::string next;
    bool next_structural = false;
    status = ParseToken(&next, &next_structural);
    if (status != Status::kOk) return Status::kError;  // key without value, or bad string
    if (next_structural && next == "}") return Status::kError;  // a key without its value
    if (++nodes_ > kMaxVdfNodes) return Status::kError;
    if (next_structural) {
      if (depth + 1 > kMaxVdfDepth) return Status::kError;
      VdfNode child;
      child.is_dict = true;
      if (ParseEntries(&child, depth + 1) == Status::kError) return Status::kError;
      SkipWhitespaceAndComments();
      if (pos_ >= text_.size() || text_[pos_] != '}') return Status::kError;  // missing }
      ++pos_;
      parent->children.emplace_back(std::move(key), std::move(child));
      return Status::kOk;
    }
    VdfNode leaf;
    leaf.value = std::move(next);
    parent->children.emplace_back(std::move(key), std::move(leaf));
    return Status::kOk;
  }

  Status ParseEntries(VdfNode* node, std::size_t depth) {
    for (;;) {
      const Status status = ParsePair(node, depth);
      if (status == Status::kError) return status;
      if (status == Status::kNone) return Status::kOk;
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t nodes_ = 0;
};

}  // namespace

const VdfNode* VdfNode::Find(std::string_view key) const {
  if (!is_dict) return nullptr;
  for (const auto& entry : children) {
    if (EqualsAsciiNoCase(entry.first, key)) return &entry.second;
  }
  return nullptr;
}

std::optional<std::string_view> VdfNode::String(std::string_view key) const {
  const VdfNode* node = Find(key);
  if (node == nullptr || node->is_dict) return std::nullopt;
  return std::string_view(node->value);
}

std::optional<std::uint64_t> VdfNode::Uint64(std::string_view key) const {
  const auto text = String(key);
  if (!text || text->empty()) return std::nullopt;
  std::uint64_t value = 0;
  const char* end = text->data() + text->size();
  const auto result = std::from_chars(text->data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
  return value;
}

std::optional<VdfNode> ParseVdf(std::string_view text) {
  if (text.size() > kMaxVdfBytes) return std::nullopt;
  if (text.find('\0') != std::string_view::npos) return std::nullopt;
  VdfNode root;
  Parser parser(text);
  if (parser.ParseTop(&root) != Status::kOk) return std::nullopt;
  return root;
}

}  // namespace playden::steam

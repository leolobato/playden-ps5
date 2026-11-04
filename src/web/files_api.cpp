// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/files_api.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <unistd.h>
#include <vector>

namespace playden::web {
namespace {

const std::vector<std::pair<std::string, std::string>> kNoStore = {{"Cache-Control", "no-store"}};
constexpr const char* kCookie = "playden_files";

std::int64_t SteadySeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Quoted(std::string_view text) {
  std::string out = "\"";
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (c == '"' || c == '\\') { out += '\\'; out += raw; }
    else if (c < 0x20) {
      char code[8];
      std::snprintf(code, sizeof code, "\\u%04x", c);
      out += code;
    } else out += raw;
  }
  return out + "\"";
}

std::string Field(std::string_view name, std::string_view text) { return Quoted(name) + ":" + Quoted(text); }
std::string Field(std::string_view name, bool value) { return Quoted(name) + (value ? ":true" : ":false"); }
std::string Number(std::string_view name, std::uint64_t value) { return Quoted(name) + ":" + std::to_string(value); }
std::string Signed(std::string_view name, std::int64_t value) { return Quoted(name) + ":" + std::to_string(value); }

void SendJson(Response& response, int status, const std::string& body,
              std::vector<std::pair<std::string, std::string>> extra = kNoStore) {
  response.Send(status, "application/json", body, extra);
}

void Fail(Response& response, const FileError& error) {
  std::vector<std::pair<std::string, std::string>> headers = kNoStore;
  if (error.status == 503) headers.emplace_back("Retry-After", "1");
  response.Send(error.status, "text/plain; charset=utf-8", error.message + "\n", headers);
}

// A game's setup holds the path lock for minutes, so a file action answers now, not after the short wait for the lock.
void SendSettingUp(Response& response) {
  auto headers = kNoStore;
  headers.emplace_back("Retry-After", "5");
  headers.emplace_back("X-Playden-Busy", "setup");
  response.Send(503, "text/plain; charset=utf-8", "Playden is setting up a game. Files are available when it finishes.\n", headers);
}

// A cookie's value from the Cookie header.
std::string CookieValue(const Request& request, std::string_view name) {
  const auto header = HeaderValue(request, "cookie");
  if (!header) return {};
  std::string_view text = *header;
  while (!text.empty()) {
    const auto semicolon = text.find(';');
    auto pair = text.substr(0, semicolon);
    text = semicolon == std::string_view::npos ? std::string_view() : text.substr(semicolon + 1);
    while (!pair.empty() && pair.front() == ' ') pair.remove_prefix(1);
    const auto equals = pair.find('=');
    if (equals != std::string_view::npos && pair.substr(0, equals) == name) return std::string(pair.substr(equals + 1));
  }
  return {};
}

// RFC 5987: attr-chars stay, everything else is %XX of its UTF-8 bytes.
std::string ContentDisposition(std::string_view name) {
  std::string out = "attachment; filename*=UTF-8''";
  for (const char raw : name) {
    const auto c = static_cast<unsigned char>(raw);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out += raw;
    else {
      char code[4];
      std::snprintf(code, sizeof code, "%%%02X", c);
      out += code;
    }
  }
  return out;
}

// A game's file must never run on the origin that holds the cookie: always a download, always sandboxed.
std::vector<std::pair<std::string, std::string>> DownloadHeaders(std::string_view name) {
  return {{"Content-Disposition", ContentDisposition(name)},
          {"Content-Security-Policy", "sandbox"},
          {"Cache-Control", "no-store"}};
}

std::string EntryJson(const FileEntry& entry) {
  std::string out = "{" + Field("name", entry.name) + "," + Field("directory", entry.directory) + "," + Number("size", entry.size) + "," +
                    Signed("modified", entry.modified) + "," + Field("read_only", entry.read_only);
  if (entry.recoverable) out += "," + Field("recoverable", true);
  if (entry.previous)
    out += ",\"previous\":{" + Number("size", entry.previous->size) + "," + Signed("modified", entry.previous->modified) + "}";
  return out + "}";
}

std::string GameJson(const FileGame& game) {
  std::string out = "{" + Field("id", game.id) + "," + Field("name", game.name) + "," + Field("kind", game.kind) + "," + Number("app", game.app) + "," +
                    Field("drive", game.drive) + "," + Field("drive_absent", game.drive_absent) + "," + Field("installed", game.installed) + "," +
                    Field("running", game.running) + "," + Field("setting_up", game.setting_up) + "," + Field("cloud", game.cloud);
  if (game.last_played) out += "," + Signed("last_played", *game.last_played);
  if (game.windows_user_bytes) out += "," + Number("windows_user_bytes", *game.windows_user_bytes);
  if (game.physx_needed) out += "," + Field("physx_needed", *game.physx_needed);
  return out + "}";
}

FileStore::Options StoreOptions(const FilesConfig& config) {
  auto options = config.store;
  if (config.try_hold) options.try_hold = config.try_hold;
  if (config.free_space) options.free_space = config.free_space;
  return options;
}

}  // namespace

// One of the few transfers that may run at once. Lists and small actions don't count.
class FilesApi::Transfer {
 public:
  explicit Transfer(std::atomic<int>& count) : count_(count) { held_ = count_.fetch_add(1) < kMaxTransfers; }
  ~Transfer() { count_.fetch_sub(1); }
  bool held() const { return held_; }

 private:
  std::atomic<int>& count_;
  bool held_ = false;
};

FilesApi::FilesApi(FilesConfig config)
    : config_(std::move(config)), pairing_(config_.pairing), store_(StoreOptions(config_)) {}

void FilesApi::SetCatalog(FileCatalog catalog) {
  std::lock_guard<std::mutex> lock(mutex_);
  catalog_ = std::move(catalog);
}

std::string FilesApi::PairingCode() const { return pairing_.ActiveCode(); }

void FilesApi::CancelPairing() { pairing_.Cancel(); }

bool FilesApi::InUse() const {
  const auto used = last_used_.load();
  return used != 0 && SteadySeconds() - used < 30;
}

bool FilesApi::Paired(const Request& request) {
  const auto token = CookieValue(request, kCookie);
  if (token.empty() || !pairing_.Paired(token)) return false;
  last_used_.store(SteadySeconds());
  return true;
}

bool FilesApi::RequirePaired(const Request& request, Response& response) {
  if (Paired(request)) return true;
  SendError(response, 401, "Pair this browser first.");
  return false;
}

// A custom header makes a browser ask first (a CORS preflight, never answered), so another web page can't send it.
bool FilesApi::RequireAction(const Request& request, Response& response) {
  if (HeaderValue(request, "x-playden-action") == std::string("files")) return true;
  SendError(response, 403, "Send X-Playden-Action: files.");
  return false;
}

std::optional<FileGame> FilesApi::Game(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& game : catalog_.games)
    if (game.id == id) return game;
  return std::nullopt;
}

bool FilesApi::SettingUp() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_.setting_up;
}

std::vector<std::pair<std::string, FileRoot>> FilesApi::Roots(const FileGame& game) const {
  std::vector<std::pair<std::string, FileRoot>> roots;
  if (!game.bottle.empty()) {
    const auto bottle_root = [&](const char* id, const std::string& below) {
      FileRoot root;
      root.path = game.bottle + "/" + below;
      root.create_from = game.bottle;
      if (std::string(id) == "user") root.private_paths = config_.private_profile_paths;
      roots.emplace_back(id, std::move(root));
    };
    bottle_root("user", "drive_c/users/prospero");
    bottle_root("steam", "drive_c/Program Files (x86)/Steam/userdata/0");
    bottle_root("programdata", "drive_c/ProgramData");
    bottle_root("public", "drive_c/users/Public/Documents");
  }
  if (!game.game_folder.empty() && !game.drive_absent) {
    FileRoot root;
    root.path = game.game_folder;
    root.folder_mode = 0777;
    root.file_mode = 0666;
    root.game_folder = true;
    roots.emplace_back("game", std::move(root));
  }
  return roots;
}

bool FilesApi::Find(const Request& request, Response& response, FileGame* game, FileRoot* root) {
  const auto id = QueryValue(request.query, "game");
  const auto named = QueryValue(request.query, "root");
  const auto found = id ? Game(*id) : std::nullopt;
  if (!found) {
    SendError(response, 404, "No such game.");
    return false;
  }
  if (SettingUp()) {
    SendSettingUp(response);
    return false;
  }
  *game = *found;
  for (auto& [root_id, value] : Roots(*found))
    if (named == root_id) {
      *root = std::move(value);
      return true;
    }
  SendError(response, 404, found->drive_absent ? "Connect this game's drive." : "This game has no such folder.");
  return false;
}

void FilesApi::Register(Router& router) {
  router.Post("/api/pair/start", [this](const Request& request, const std::string&, Response& response) {
    if (!RequireAction(request, response)) return;
    std::int64_t wait = 0;
    const bool replace = QueryValue(request.query, "new") == std::string("1");
    if (!pairing_.Start(replace, &wait)) {
      if (wait > 0) {
        response.Send(429, "text/plain; charset=utf-8", "Too many wrong codes. Try again in a moment.\n",
                      {{"Retry-After", std::to_string(wait / 1000 + 1)}, {"Cache-Control", "no-store"}});
        return;
      }
      return SendError(response, 500, "Playden couldn't make a code.");
    }
    SendJson(response, 200, "{\"code_on_tv\":true}");
  });
  router.Post("/api/pair/confirm", [this](const Request& request, const std::string&, Response& response) {
    if (!RequireAction(request, response)) return;
    std::string token;
    switch (pairing_.Confirm(QueryValue(request.query, "code").value_or(std::string()), &token)) {
      case Pairing::ConfirmResult::kPaired:
        SendJson(response, 200, "{\"paired\":true}",
                 {{"Set-Cookie", std::string(kCookie) + "=" + token + "; HttpOnly; SameSite=Strict; Path=/"},
                  {"Cache-Control", "no-store"}});
        return;
      case Pairing::ConfirmResult::kWrong: return SendError(response, 403, "That isn't the code on the TV.");
      case Pairing::ConfirmResult::kLocked: return SendError(response, 429, "Too many wrong codes. Try again in a moment.");
      case Pairing::ConfirmResult::kNoCode: return SendError(response, 409, "That code ended. Ask for a new one.");
      case Pairing::ConfirmResult::kFailed: return SendError(response, 500, "Playden couldn't finish pairing.");
    }
  });
  router.Exact("/api/pair/status", [this](const Request& request, const std::string&, Response& response) {
    SendJson(response, 200, "{" + Field("paired", Paired(request)) + "}");
  });

  router.Exact("/api/files/games", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    FileCatalog catalog;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      catalog = catalog_;
    }
    std::string body = "{" + Field("known", catalog.known) + "," + Field("setting_up", catalog.setting_up) + ",\"games\":[";
    for (std::size_t i = 0; i < catalog.games.size(); ++i) body += (i ? "," : "") + GameJson(catalog.games[i]);
    SendJson(response, 200, body + "]}");
  });
  router.Exact("/api/files/game", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    const auto id = QueryValue(request.query, "game");
    const auto game = id ? Game(*id) : std::nullopt;
    if (!game) return SendError(response, 404, "No such game.");
    std::string body = GameJson(*game);
    body.pop_back();
    body += ",\"game_folder\":" + Quoted(game->game_folder_shown) + ",\"roots\":[";
    bool first = true;
    std::vector<RedistFolderInfo> redist;
    const bool setting_up = SettingUp();
    bool redist_busy = false;
    // Once the lock is busy, the other roots would each wait for it too.
    bool busy = setting_up;
    for (const auto& [root_id, root] : Roots(*game)) {
      FileListing listing;
      // Only whether it exists: a busy lock leaves it unknown, and the page can still open it.
      const auto error = busy ? FileError{503, std::string()} : store_.List(root, std::string(), &listing);
      if (error.status == 503) busy = true;
      body += std::string(first ? "" : ",") + "{" + Field("id", root_id) + "," + Field("exists", error.ok() ? listing.exists : true) + "}";
      first = false;
      // Asked for by the game's own page only: a look through a game folder is too slow for every folder opened.
      if (root_id == "game" && QueryValue(request.query, "redist") == std::string("1") && (busy || store_.RedistFolders(root, &redist).status == 503))
        redist_busy = true;
    }
    body += "],\"redist\":[";
    for (std::size_t i = 0; i < redist.size(); ++i) {
      const auto& folder = redist[i];
      body += std::string(i ? "," : "") + "{" + Field("path", folder.path) + "," + Number("entries", folder.entries) + "," + Number("bytes", folder.bytes) + "," +
              Field("physx_installer", folder.physx_installer) + ",\"sample\":[";
      for (std::size_t j = 0; j < folder.sample.size(); ++j) body += (j ? "," : "") + Quoted(folder.sample[j]);
      body += "]}";
    }
    SendJson(response, 200, body + "]," + Field("redist_busy", redist_busy) + "," + Field("all_setting_up", setting_up) + "}");
  });
  router.Exact("/api/files/list", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    FileListing listing;
    const auto error = store_.List(root, QueryValue(request.query, "path").value_or(std::string()), &listing);
    if (!error.ok()) return Fail(response, error);
    std::string body = "{" + Field("exists", listing.exists) + "," + Field("read_only", listing.read_only) + "," + Field("truncated", listing.truncated) + "," +
                       Number("unshown", listing.unshown) + ",\"entries\":[";
    for (std::size_t i = 0; i < listing.entries.size(); ++i) body += (i ? "," : "") + EntryJson(listing.entries[i]);
    SendJson(response, 200, body + "]}");
  });
  router.Exact("/api/files/download", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    Transfer transfer(transfers_);
    if (!transfer.held()) return Fail(response, {503, "Other transfers are running. Try again in a moment."});
    OpenedFile file;
    const auto error = store_.OpenForRead(root, QueryValue(request.query, "path").value_or(std::string()),
                                          QueryValue(request.query, "previous") == std::string("1"), &file);
    if (!error.ok()) return Fail(response, error);
    if (response.Start(200, "application/octet-stream", file.size, DownloadHeaders(file.name)) && request.method != Method::kHead) {
      std::vector<char> buffer(256 * 1024);
      std::uint64_t left = file.size;
      while (left > 0) {
        const auto count = ::read(file.fd, buffer.data(), static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size())));
        if (count <= 0 || !response.Body(buffer.data(), static_cast<std::size_t>(count))) break;
        left -= static_cast<std::uint64_t>(count);
      }
    }
    ::close(file.fd);
  });
  router.Exact("/api/files/zip", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    Transfer transfer(transfers_);
    if (!transfer.held()) return Fail(response, {503, "Other transfers are running. Try again in a moment."});
    const auto path = QueryValue(request.query, "path").value_or(std::string());
    const auto last_slash = path.rfind('/');
    const auto base = path.empty() ? game.name : path.substr(last_slash == std::string::npos ? 0 : last_slash + 1);
    bool started = false;
    const auto modified = config_.now ? config_.now()
                                      : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto error = store_.WriteZip(
        root, path, modified, [&response](const char* data, std::size_t size) { return response.Body(data, size); },
        [&] {
          auto headers = DownloadHeaders(base + ".zip");
          return response.StartChunked(200, "application/zip", headers) && request.method != Method::kHead;
        },
        &started);
    if (!error.ok() && !started) return Fail(response, error);
    // Cut without the final chunk on a failure: the browser shows the download as failed.
    if (error.ok() && started) response.FinishChunks();
  });
  router.Exact("/api/files/text", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response)) return;
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    FileText text;
    const auto error = store_.ReadText(root, QueryValue(request.query, "path").value_or(std::string()), &text);
    if (!error.ok()) return Fail(response, error);
    SendJson(response, 200, "{" + Field("sha256", text.sha256) + "," + Field("text", text.text) + "}");
  });

  router.Post("/api/files/upload", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response) || !RequireAction(request, response)) return;
    if (!request.content_length) return SendError(response, 411, "Send the file's length.");
    if (!request.read_body) return SendError(response, 400, "No body.");
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    Transfer transfer(transfers_);
    if (!transfer.held()) return Fail(response, {503, "Other transfers are running. Try again in a moment."});
    UploadOptions options;
    options.length = *request.content_length;
    options.overwrite = QueryValue(request.query, "overwrite") == std::string("1");
    if (auto hash = QueryValue(request.query, "if_sha256"); hash && !hash->empty()) {
      options.if_sha256 = std::move(*hash);
      options.overwrite = true;
    }
    bool replaced = false;
    const auto error = store_.Upload(root, QueryValue(request.query, "path").value_or(std::string()), options, request.read_body, &replaced);
    if (!error.ok()) return Fail(response, error);
    SendJson(response, 200, "{" + Field("replaced", replaced) + "}");
  });
  const auto action = [this](FileError (FileStore::*work)(const FileRoot&, const std::string&)) {
    return [this, work](const Request& request, const std::string&, Response& response) {
      if (!RequirePaired(request, response) || !RequireAction(request, response)) return;
      FileGame game;
      FileRoot root;
      if (!Find(request, response, &game, &root)) return;
      const auto error = (store_.*work)(root, QueryValue(request.query, "path").value_or(std::string()));
      if (!error.ok()) return Fail(response, error);
      SendJson(response, 200, "{\"ok\":true}");
    };
  };
  router.Post("/api/files/mkdir", [this](const Request& request, const std::string&, Response& response) {
    if (!RequirePaired(request, response) || !RequireAction(request, response)) return;
    FileGame game;
    FileRoot root;
    if (!Find(request, response, &game, &root)) return;
    const auto error = store_.MakeFolder(root, QueryValue(request.query, "path").value_or(std::string()),
                                         QueryValue(request.query, "parents") == std::string("1"));
    if (!error.ok()) return Fail(response, error);
    SendJson(response, 200, "{\"ok\":true}");
  });
  router.Post("/api/files/delete", action(&FileStore::Delete));
  router.Post("/api/files/restore", action(&FileStore::Restore));
}

}  // namespace playden::web

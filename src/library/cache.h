#pragma once
#include "library/repository.h"
#include <string>
#include <utility>

namespace playden {
class LibraryCache final : public LibraryRepository {
public:
    // Files are <file_prefix><decimal account>.cache: Steam's "library-", GOG's "library-gog-" (its account is a hash
    // of the GOG user ID, never the ID itself).
    explicit LibraryCache(Storage& storage, std::string file_prefix = "library-")
        : storage_(storage), file_prefix_(std::move(file_prefix)) {}
    Result<LibrarySnapshot> Load(std::uint64_t account_id) const override;
    Result<void> Save(std::uint64_t account_id, const LibrarySnapshot& snapshot) const override;
    Result<void> Clear(std::uint64_t account_id) const override;
    // Kept for existing callers that already hold the canonical decimal ID.
    Result<LibrarySnapshot> Load(const std::string& account_id) const;
    Result<void> Save(const std::string& account_id, const LibrarySnapshot& snapshot) const;
    Result<void> Clear(const std::string& account_id) const;
private:
    std::string Path(const std::string& account) const { return file_prefix_+account+".cache"; }
    Storage& storage_;
    std::string file_prefix_;
};
}

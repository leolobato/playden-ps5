# Steam store user tag filtering (development)

Work in progress on branch **`feature/steam-store-tag-filtering`**. This is not in a release yet.

## Get the branch (Mac)

Official host builds are macOS-only; see [building.md](building.md).

```sh
git fetch origin
git checkout feature/steam-store-tag-filtering
git submodule update --init --recursive
scripts/fetch-deps.sh
```

Keep **`third_party/prospero-win`** clean at the pinned revision (configure fails if the submodule is dirty). Do not commit local prospero changes with this feature.

## Build and test on macOS

```sh
scripts/build-homebrew.sh
cmake --build --preset homebrew-host
ctest --preset homebrew-host --output-on-failure
```

Focused tests:

```sh
ctest --preset homebrew-host -R 'store_tags|homebrew_browse|homebrew_library_model|library_cache|library_organization|store_details' --output-on-failure
```

## Simulate in the UI (no console)

```sh
build/homebrew-host/playden-homebrew --simulation --scenario default
```

Fixtures **`fixtures/ui-comparison/catalog.json`** include `store_tags` on games **900001** (Roguelike, Adventure) and **900002** (Adventure):

1. Open **Library** → first game → **Details**.
2. Select a **store tag** row (not the grey genre chips).
3. Library filters to games with that tag; **Sort and filters** shows the active tag and **Clear tag filter**.

Live mode fills tags from the network (`StoreTagsService`); simulation loads tags from the catalog only.

## PS5 package

```sh
scripts/build-lapy-helper.sh
scripts/build-homebrew-ps5.sh
```

Output: `build/homebrew-ps5/dist/FAKE77031` → upload to `/data/homebrew/FAKE77031` on the console (FTP/zftpd on port 2120, or FileZilla).

Deploy when FTP works:

```sh
python3 tools/console_title_deploy.py --host <PS5 IP> deploy build/homebrew-ps5/dist/FAKE77031 \
  --backup "build/console-backups/ps5-$(date +%Y%m%d-%H%M%S)"
```

First install on a console with no title folder: add **`--install`**.

## Console behaviour

- Tags are **not** in the Steam library sync; they load in the background from store pages (throttled), then persist in **library cache schema 8**.
- **Details** shows tag rows only when `store_tags_known` is true. Until enrichment runs, there is nothing to tap.
- With an active tag filter, games without tag data are **hidden**.
- Genres on Details remain separate from user tags.

If tags never appear: stay signed in on Library for a few minutes, then check Details again; collect logs with `tools/collect_logs.py --host <PS5 IP>` when FTP is up.

## What this branch changes (overview)

- `StoreTagsService` — parse `"tags":[…]` from store HTML.
- Library cache **schema 8** — `store_tags` / `store_tags_known`.
- Organization file **v3** — optional `tag_filter`.
- Browse filter, search-by-tag, Details actions, Sort and filters clear row.

Artwork cache and TTL are unchanged; tag metadata does not use `art/`.

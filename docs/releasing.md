# Publishing a release

Maintainer steps. Contributors do not need this page.

Playden updates itself from the latest GitHub release (`releases/latest`, which leaves out drafts and
pre-releases). Players who enable Beta updates use the releases list, including betas. Every release needs its
update manifest:

1. Raise the version in `CMakeLists.txt` (`project(PlaydenLauncher VERSION …)`) and build the PS5 package. The
   version becomes `param.json`'s `contentVersion` and the version Playden compares.
2. Make the zips:
   - `playden-ps5-<version>.zip`: the `FAKE77031` folder at the top of the archive, with `tools/` (the console
     tools), `README.md`, `COMPATIBILITY.md`, `LICENSE` and `THIRD_PARTY_NOTICES.md` next to it. The ProsperoStore
     catalog needs the app folder at the top (see its
     [artifact formats](https://github.com/darkxex/ps5-homebrew-catalog/blob/main/docs/artifact-formats.md)) and
     ignores the rest; a store install copies only the app folder, so the package build puts `LICENSE` and
     `THIRD_PARTY_NOTICES.md` inside it too. Playden 0.6.0 and newer unpack the folder the manifest's `prefix` names,
     so they update from this layout as well. Releases up to 0.7.1 had everything inside `playden-ps5-<version>/`.
   - `playden-kit-<kit version>.zip` (with the kit folder, named after its `kit_id`, inside
     `playden-kit-<kit version>/`, for example `playden-kit-0.13/playden-kit/`).
3. Write the manifest:

   ```sh
   python3 tools/write_update_manifest.py --app-zip playden-ps5-<version>.zip \
       --kit-zip playden-kit-<kit version>.zip --kit-revision <kit version> --min-from <oldest version> \
       --notes "A short summary of the release." --out playden-update.json
   ```

   `--notes` is the summary the update dialog shows: plain text (no Markdown) of at most 400 characters, such as the
   first line of the release notes. The dialog links to the release page, as a QR code, for the full notes. The tool
   refuses a kit that `dependencies.lock.json` does not pin. `--min-from` is the oldest version that can update
   to this one; older versions are told to install by hand.
4. Attach `playden-update.json` and the zips to the release, then publish it. Never replace an asset of a
   published release: the ProsperoStore catalog pins it by its SHA-256. GitHub records each file's SHA-256,
   and Playden checks the downloads against it. If the kit did not change, the kit zip may be left out: consoles
   that already have that kit download only the app, and the others take the zip from an older release that has it.
   Playden 0.7.0 and older look only in the new release, so keep the zip while a version that old can update to it
   without having the kit (or raise `--min-from`).

## Betas

Keep the numeric version in `project()` (for example `0.7.1`) and configure with `-DPLAYDEN_PRERELEASE=beta.1`. The
package writes `playden-version` as `0.7.1-beta.1`; About, the updater and log bundles use it. PS5's `param.json`
cannot represent a pre-release: `contentVersion` stays `00.007.001` for all 0.7.1 betas and the stable release. Set
`PLAYDEN_PRERELEASE` back to empty for stable builds.

Name the app zip `playden-ps5-0.7.1-beta.1.zip`, pass `--tag v0.7.1-beta.1` to the manifest tool to verify it matches
the package, and **mark the GitHub release as a pre-release; never use `--latest` for a beta**. `--min-from` may name
a beta. The kit pin and trust rules are the same as for stable releases.

## Try an update without publishing

Put `release.json` (from `--release-json`, with `--allow-other-kit` for a test kit), `playden-update.json` and the
zips in `/data/homebrew/FAKE77031/dev/update-local/`. Playden then reads the release from there instead of GitHub.
Delete the folder afterwards.

## Maintainer notes

The maintainer's status notes, plans and console evidence are kept in a separate repository. Comments in the code
that name a `results/<run>` folder refer to it.

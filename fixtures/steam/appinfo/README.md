# Steam appinfo fixtures

Text VDF as `ParseAppInfo` takes it (root `"appinfo"`), trimmed to the keys depot selection reads.

- `factorio.vdf`: shaped after Factorio (427520): 64- and 32-bit Windows depots, Mac and Linux depots, and its
  Space Age DLC (645390) listed in the game's own depots by `dlcappid`. Manifest IDs and sizes are illustrative.
- `portal.vdf`: shaped after Portal (400): language-neutral content, a Russian voice depot, Windows and Mac binaries.
- `dlc-base-5000.vdf`, `dlc-app-5001.vdf`, `dlc-app-5002.vdf`: synthetic. A game whose DLC apps list their own
  depots, one 64/32-bit pair, one set of language depots, and a copy of the game's content depot.
- `free-dlc-base-6000.vdf`, `free-dlc-app-6001.vdf`, `free-dlc-app-6002.vdf`: synthetic, for choosing which DLC to ask
  Steam for free. 6001 lists a Windows depot of its own, 6002 only a Mac one, 6003 has a Windows depot the game lists by
  `dlcappid`, 6004 has no depot at all and 6005 only a Mac one the game lists.
- `versions-7000.vdf`: synthetic, shaped after The Witcher 3's versions. The public branch carries the content and a
  DX12 binaries depot; branch `4.04` carries the content and an x64 binaries depot only it lists; `legacy` uses the old
  format (a bare manifest ID) for the content only; `beta` needs a password; `secret` has only encrypted manifests; and
  `macold` has a Mac depot only.

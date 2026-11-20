<!-- Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault -->
# AGENTS.md - working on RPCS3 for Game Vault

For AI coding agents (Claude Code, Codex, ...) and the people driving them. Read this before changing anything.

RPCS3 for Game Vault is the PS3 emulator that Game Vault starts on an Xbox Series X|S in Developer Mode. It is a fork
of emelin2004's RPCS3 for Xbox (UWP port of RPCS3, GPL-2.0). The technical side of the port is in
[uwp/README.md](uwp/README.md); per-game settings in [uwp/GAME_SETTINGS.md](uwp/GAME_SETTINGS.md).

## Branches - where your work goes

| Branch | What it is |
| --- | --- |
| `contrib` | **Base for your work and target of your pull requests.** Starts at the released 0.1.35.1623 code. |
| `gamevault` | The owner's build branch (a push builds a release candidate). Do not push here. |
| `gv-*` | The owner's work in progress. Don't build on them; don't redo what they do (see "In progress"). |
| `main` | emelin2004's upstream port, merged in from time to time. Not where Game Vault work goes. |

- One fix per pull request, from a branch off `contrib`, named for what it fixes (`fix/bleach-font`).
- The owner merges, builds and releases. Never publish releases, tags or packages from this repo.
- Two AI agents must not edit the same files at once: say in the PR (or on Discord) which files you are on.

## How the picture is drawn (where most fixes go)

The Xbox only lets apps use **D3D12** (feature level 11_0, shader model 6.4). RPCS3's renderer is **Vulkan**, so it
runs on **Mesa Dozen** (`vulkan_dzn.dll`, Vulkan 1.2 on top of D3D12):

```
PS3 game -> RPCS3 RSX -> RPCS3 Vulkan renderer (rpcs3/Emu/RSX/VK) -> Dozen -> D3D12 -> Xbox GPU driver
```

So a picture bug that does not happen on RPCS3 for PC is almost always in one of two places:

1. **RPCS3's Vulkan renderer** (`rpcs3/Emu/RSX/VK/`): asking Dozen for something D3D12 can't do well. Fix it on the
   RPCS3 side when the RPCS3 side has a choice. Example (released 0.1.35.1615): Dynasty Warriors: Gundam Reborn's
   mission world was hidden behind the sky because the depth clip transform used **double** precision; D3D12 has no
   double `floor` and Dozen's lowering gave wrong depth. Fix: single-precision transform on the Xbox
   (`VKVertexProgram.cpp`, switch `gv_fp64` to go back).
2. **Dozen** (Mesa, `src/microsoft/vulkan/`). We don't fork Mesa: the build downloads Mesa (version in
   `MESA_VERSION`, `.github/workflows/build-gamevault.yml`) and applies the patches in `uwp/toolchain/mesa-dzn-*.patch`
   (`uwp/toolchain/build-mesa-dzn.sh`). A Dozen fix = a new `.patch` file plus a line in that script.

Things Dozen/D3D12 on the Xbox don't have (avoid them, or give RPCS3 another path): double-precision math
(`floor`, division), `logicOp`, wide lines, int16 in shaders, descriptor indexing ("Unsupported SPIR-V capability
RuntimeDescriptorArray" warnings in the Mesa log are known and harmless so far).

Known Xbox driver trap (found 2026-11-20 in another Game Vault emulator on Dozen): copying from CPU **sampler**
descriptor heaps bigger than 2048 samplers crashed the Xbox driver (`umd12ddi_arden.dll`, inside
`CopyDescriptorsSimple`). Keep Vulkan descriptor pools at 2048 samplers or fewer.

## Build

- **Cloud build only** (no Windows PC needed): GitHub Actions `.github/workflows/build-gamevault.yml` (Linux,
  zig/clang cross toolchain, LLVM and FFmpeg cached; about 35 min, the first LLVM build about 2 h). On your own fork
  run it with "Run workflow" on your branch. Output: artifact `rpcs3-gamevault-layout` (unsigned package folder).
- Packing and signing: `uwp/package/pack_gamevault.ps1` is the owner's (it uses the Game Vault signing key, which
  never leaves the owner's PC - don't ask for it). To test on your own console, pack the artifact with `makeappx`
  and sign it with **your own** test certificate whose subject matches the manifest's `Publisher="CN=GamerDad"`.
- Version: the owner sets it when packing (released: 0.1.35.1623). Don't change the version in the manifest.

## Testing on a console

- The app is started **by Game Vault** (link `gamevault-rpcs3:?cmd=rpcs3 "<game path>"&launchOnExit=...`).
  Device Portal's "launch" always fails for this app (0x8027025B) - that is not a bug.
- App type must be **Game** (5 GB of memory; App mode has 1 GB). Big games still hit 5 GB (Blur's first race,
  Watch Dogs): graphics heaps count in it.
- Firmware installs by itself (PUP from a drive, else Sony's update server). Caches go to the USB drive
  (`<USB>/RPCS3/cache`), because the app's own storage is tiny.
- Per-game settings: built in (`uwp/app/uwp_main.cpp`, `get_database_config`) or
  `LocalState/rpcs3/custom_configs/config_<serial>.yml`.
- Test switches: an empty file with this name in the app's LocalState (Device Portal > File explorer):

| File | Effect |
| --- | --- |
| `gv_fp64` | double-precision depth clip transform again (the PC behaviour) |
| `gv_drawlog` | draw-state log every 3 s |
| `gv_nocull` / `gv_nodepth` / `gv_noclamp` | no culling / no depth test / no depth clamp (to find hidden geometry) |
| `gv_nocondrender` | every draw made (occlusion-query conditional rendering ignored) |
| `autopress` | scripted pad presses: lines `"<seconds> btn[+btn] [ms]"` or `"repeat a b every btn [ms]"`, from game start; buttons cross circle square triangle start select up down left right l1 r1 l2 r2 l3 r3 |

- Logs: RPCS3's log (Dozen's warnings land there too); Game Vault players' reports reach the owner.
- Always say what you tested on (console model, game serial, build) and what you saw. Never guess a game works.

## Games - what is known (don't redo)

- **Bleach: Soul Resurreccion** (BLUS30769): plays, 60 fps menus / 30 in levels (game cap). Text is blocky/fat
  (fine on PC; also emelin2004/rpcs3-xbox#93). Ruled out: LOD bias, HLE font (it uses LLE libfont), strict
  rendering. Thought to be on the Dozen side. Open - a good first target.
- **Blur** (BLUS30295): needs Write Color Buffers, SPU blocks Safe + loop detection, 4 SPURS threads (all built
  in). Open: first race runs out of memory at 100% resolution (4.7/5 GB: D3D12 heaps ~1.2 GB + placed resources
  ~1.7 GB). At 50% it loads but runs 4-5 fps (CPU-bound). A VRAM cap of 1024 made it worse.
- **Dynasty Warriors: Gundam Reborn** (BLES02057): fixed (single-precision depth, above).
- Upstream list for the port (emelin2004's README): Minecraft ~57, Portal 2 ~28, Jetpack Joyride 60, GTA V ~10.

## In progress by the owner (don't duplicate)

- Saves failing with "disk full" while there is free space (Bleach autosave) - branch `gv-write-full`.
- Encrypted disc images read with an `.ird` file - branch `gv-ird`.

## Rules

- Every source file you add or substantially change carries this notice at the top (keep the existing ones):
  `Copyright (c) 2026 Skitzelles - part of Game Vault, https://github.com/skitzellez/xbox-game-vault`
- Licences: RPCS3 is GPL-2.0; keep upstream licence headers; only add code you may license under GPL-2.0.
- Fix it for every game when you can (in the renderer or Dozen), not with a per-game hack. A per-game setting is fine
  when the game itself needs it (as on PC).
- Check RPCS3's and emelin2004's issue trackers first: a bug already known upstream is fixed there, not here.
- Diagnostic logging that helps with player reports may stay in builds (short, not every frame).
- No secrets, keys, private paths or personal details in commits, logs or PR text.
- Commit messages say what changed and why, in plain words, starting `RPCS3 for Game Vault:`.

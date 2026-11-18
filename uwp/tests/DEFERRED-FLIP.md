# Deferred PS3-native flip queue regression

Spartan contribution: PSYCHOsmurfZA, with OpenAI Codex assistance. Based on the public Game Vault gv-0.1.35.1623 tag, 09d49cba5b743819632959c70aac8ff4fe606300. This is a shared display-engine correction, not a Bleach-specific override.

## Demonstrated defect

`display_engine::handle_emu_flip` popped the queued frame before checking whether PS3-native vsync could present it. If the flip had to wait for the next vblank, the first call consumed its queue entry and returned. Every retry then found the queue empty, called `on_frame_end(..., true)` again and replaced the queued statistics. End-of-frame work includes memory/zcull bookkeeping, profiling and queueing; it is not a harmless read.

The host regression extracts the production method prelude through the eligibility/queue handling and the production display queue struct. At the baseline it fails with `deferred retries re-finalize frames`. With the correction, 10,000 retries preserve the original queued frame and metadata, and a manually requested frame finalizes once when accepted. No-vsync, ordinary frame limiting and recursion guards are also checked. No Vulkan API, real vblank thread or GPU work is emulated by the harness.

## Change and invariants

Consume or synthesize the queue entry only after the timing/vblank eligibility checks succeed. Keep frame-limit selection, vblank condition, notification handling, GPU submission, shaders, descriptor lifetimes, saves and settings unchanged. No new tuning setting or per-game condition.

Test package source version: 0.1.35.11. This is a Spartan test version, not a Game Vault public release; packaging must use the separate test identity and must not overwrite the installed Game Vault app.

## Evidence and limits

The live official-build Bleach BCAS20186 level-start capture uses PS3 Native, VBlank60 and Vulkan. It shows impossible 1000+ FPS diagnostic intervals and repeated empty/minimal frame statistics, consistent with repeated frame finalization. It also shows 3733-draw text frames with 0.8–2 second stalls and long presentation waits, even with no new pipeline completions. Those waits are not proven caused by this queue defect. Earlier warm Bleach captures with Auto limiting show GPU command-chunk driver waits and are a distinct unresolved path.

The patch fixes a demonstrated queue/bookkeeping defect. Host test and cross-build success cannot establish improved popup performance; an equal-scene device comparison remains required. Do not claim the Bleach issue solved until measured.

Run: `python3 uwp/tests/check-deferred-flip.py` (C++20 host compiler; optional `--cxx`).

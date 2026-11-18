# UWP pipeline eviction accounting

Spartan work: PSYCHOsmurfZA / PSYCHOsmurf-beep, with OpenAI Codex assistance. Gemini's independent handoff identified the placeholder-accounting issue and proposed moving disposal out of the cache mutex. This change is reconstructed against the actual source; upstream authorship and licenses remain unchanged.

## Reproduced defect

`get_graphics_pipeline()` inserts a null map entry while compilation is pending. The eviction routine counted those entries toward its live-pipeline limit, while choosing only completed pipelines as eviction candidates. A cache with two completed pipelines and 38 pending entries could evict both completed pipelines despite a live limit of two.

`python3 uwp/tests/check-pipeline-eviction.py` extracts the production eviction routine into a host boundary harness. It failed on the original 2b03dc7 routine with `pending placeholders evict valid live pipelines`. The corrected routine passes this case, pinned-pipeline preservation, oldest-first selection, idle protection, disabled limits, an empty cache and deferred-disposal callbacks outside the map mutex.

## Change

- Read map size under its mutex and count completed non-null pipelines for budget decisions.
- Preserve pending entries, the two pinned pipeline pointers and the existing idle/LRU policy.
- Transfer evicted ownership to a temporary vector under the mutex; then call the existing deferred GPU garbage collector outside that mutex. Candidate scanning and sorting still hold the mutex.
- Keep the UWP-only guard, default 512-pipeline budget and 10-second idle threshold. Desktop behavior is unchanged.

The host harness validates accounting and ownership flow, not Vulkan/D3D12 integration, GPU completion safety or a performance gain. Windows cross-compilation and Xbox tests are separate requirements. The installed 0.1.35.9 diagnostic build does not include this change.

## Findings requiring more evidence

Positive draw counts and zero new pipeline completions do not rule out other dropped draws. Use the actual unavailable-pipeline counters from 0.1.35.9 during the reproduced fault. Nonzero fence waits exist in the earlier log, so they cannot be excluded globally. GPU heap headroom does not establish UWP process-memory headroom; do not quadruple the live-pipeline budget based on it.

The omitted blend state in the hash is a distribution/performance candidate, not proof of incorrect pipeline selection: full key equality still compares relevant blend fields. Verify equality/hash invariants before any hash change. Usage timestamp coverage and active diagnostic logging overhead need their own controlled checks. Neither is modified here.

## Xbox comparison

Use the same game build, cache, settings, scene and characters as the 0.1.35.9 baseline. Compare actual unavailable-draw counts, new pipeline completions, eviction deltas and sampled frame waits in a fresh process and repeated scene. Preserve saves and cache. For the captured Bleach BCAS20186 baseline, the log explicitly records Vblank Rate 60 and Frame limit Auto; keep those settings. Native target frame rate and attainable Xbox emulator frame rate are different measurements.

No fix for invisible graphics, stutters or audio has yet been demonstrated on hardware.

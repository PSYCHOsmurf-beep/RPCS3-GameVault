# Xbox missing-pipeline draw evidence

The old slow-frame label `program lookups skipped` reported `program_cache_lookups_ellided / program_cache_lookups_total`. Those debug-overlay-only counters measure elided shader-cache hint lookups. They do not count discarded draw batches and cannot rule out missing geometry during asynchronous compilation.

This change counts the actual early return in `VKGSRender::end_draw()` when `load_program()` returns false. It reports `UWP pipeline readiness: missing/attempted draw batches skipped (pipeline unavailable)` at most once per second, independently of frame hitch thresholds and the debug overlay. The interval is reported explicitly; windows reset when the game run changes. Counts describe draw batches reaching end_draw, not individual GPU draws, pixels or a proven cause of visual corruption.

No shader compilation mode, rendering state, cache limit, resource lifetime or synchronization policy is changed. This is a diagnostic correction, not a stutter or rendering fix.

Run `python3 uwp/tests/check-pipeline-readiness.py` to compile and execute the production draw-entry/early-return prefix and frame-statistics declaration under both UWP and desktop defines. The test also exercises the production aggregation helper across ready/missing batches, frame reset, sub-second/boundary timing, game changes, clock rollback, empty intervals and zero-miss windows. Full Xbox compilation and hardware correlation remain required.

For the next test, use a fresh app process per game. Capture J-Stars before geometry appears, after it appears, and during ghosting; compare readiness counts at those times. Zero unavailable counts during a reproduced fault rejects missing pipelines as its immediate cause in that interval. Nonzero counts support discarded work but alone do not prove which pixels were affected. Keep captures and game/scene identity alongside logs.

Prepared for PSYCHOsmurfZA / PSYCHOsmurf-beep with OpenAI Codex assistance; upstream authorship and licenses remain unchanged.

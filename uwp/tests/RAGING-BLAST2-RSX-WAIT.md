# Raging Blast 2: discriminate the shared loading freeze

Candidate package 0.1.35.14, based on installed diagnostic e49a800 (.13).
This is instrumentation, not a freeze/performance fix. The pending .12 presentation
change remains in a different worktree and is not included.

## Evidence driving this change

The official Game Vault package .1623 also freezes with the same USB ISO,
BLES00978 APP_VER/VERSION 02.00, PS3 Native and VSync Disabled. Its second boot in
the preserved `official-matched-20261119T075340Z` capture uses `F:/GAMES/`; the
first boot in that file used NAS/Adaptive and is not the matched control.
Official and .13 used configurations differ only in generated system identity.
The apps still differ in cache/dev_hdd1 location and per-app firmware context;
this does not establish identical environments or validate the game dump.

Official second boot resumes two frames after a 29.676-second FIFO-idle gap, then
has an empty RSX FIFO and a main PPU inside `sys_rsx_context_attribute`. That
function's hot calls are trace-filtered, so existing logs do not identify the
operation or whether it waits on a context lock, frame-end work or event delivery.
The .13 trophy trace separately proves installation/registration can return;
callback backlog alone is not evidence that trophy handling caused the freeze.

## Added observations

- Per-PPU active RSX call: six original arguments, phase and call age, published
  through atomic fields with sequence validation. Nested calls restore their
  outer record. No per-call log writes, guest-register edits or waits.
- A separate record for non-CPU VBlank-thread package0xFED calls. Thread-local
  scope routes event-send stages to the actual sender's record; unrelated PPU/RSX
  senders cannot overwrite its phase.
- Scheduler snapshot: try-lock only; pending acknowledgements, ready flag,
  capacity, at most 64 runnable records with ack_suspend, and timed-wait deadline.
  Formatting happens after unlocking and outside ID-manager enumeration locks.
- Twenty early stall samples plus five late samples at 30/60/120/300/600 tick
  seconds. This closes the gap where all observations expired during trophy I/O.
  UI tick time is not a precise wall-clock measurement; call age uses system time.

## Next single test and interpretation

After the full build/package is verified and deployed, use the same USB game,
PS3 Native and VSync Disabled. Leave the frozen screen for at least one minute
after it appears and collect the live log; preserve it again after quitting.

- `context-lock` identifies the outer call waiting for context serialization;
  it does not identify the lock owner by itself.
- `fifo-control` identifies FIFO-control acknowledgement processing.
- `frame-end` identifies the frame-end body, not necessarily GPU execution.
- `event-scheduler-check` plus pending acknowledgements/runnable states can
  support a scheduling dependency; repeated coherent samples are required.
- An idle RSX record means no recorded RSX attribute call was active, not that
  the PPU is healthy or polling callbacks. Busy scheduler snapshots say only that
  the diagnostic could not take the lock without waiting.

No callback bypass, forced wakeup, frame-limit change, resource-lifetime change
or storage mutation is made by this patch. Do not label the freeze fixed after
host tests or a successful cross-build.

## Specific upstream hypothesis (open, unmerged)

[RPCS3 PR20100](https://github.com/RPCS3/rpcs3/pull/20100), retrieved19Nov2026,
reports a deadlock introduced by42dd822 (the context-serialization change also
present in this fork): VBlank package0xFED holds sys_rsx_mtx while send_event
retries a full queue; the consumer needs the mutex. It is explicitly test-only,
unmerged. Its reported affected games do not establish a Raging Blast2 fix.
The reviewer considers the approach sound but requests implementation cleanup.

Repeated VBlank event-full-wait snapshots plus a PPU waiting on context-lock
would support this specific cycle; inspect protected scheduler states as well.
VBlank idle/differentphase or a PPU stuck elsewhere argues against that cycle in
this capture. This build records both sides; it does not import the experimental
lock-release patch. The old20sample cap can miss the post-install state entirely.

## Checks

`python3 uwp/tests/check-stall-diagnostics.py` exercises the production atomic
record, nested/early-return cleanup, concurrent snapshot coherence, bounded late
cadence and the extracted production scheduler method's busy-lock/cap/preservation
paths. It substitutes scheduler types/formatter; it does not compile all RPCS3 or
prove Xbox timing. The native-flip regression remains a separate required check.

## Lock owner and GCM queue state (follow-up, uncommitted review build)

Evidence that motivated this: in the official log's first (unmatched) boot, main_thread
at 0x8a1bb4 left `sys_rsx_context_attribute` with CELL_EAGAIN at shutdown. That return
exists only on the `_sys_rsx_lock_context_calls` try-lock path, so main_thread was waiting
for `sys_rsx_mtx`. `_gcm_intr_thread` was sampled in `sys_event_queue_receive`, which does
not match PR 20100's exact cycle. The earlier diagnostics could not name the lock holder.

Each stall report now adds two lines:

- `UWP rsx lock: owner=<ppu|vblank|other-cpu|host> id=... site=... pkg=... held=...ms`
  is published by whichever of the six `sys_rsx_mtx` sites holds it exclusively. It is set
  after acquisition and cleared before release. The RSX engine thread is exempt and never
  publishes. "no recorded owner" means free, a shared (non-UWP kernel explorer) reader, or
  a publication racing the sample.
- `UWP rsx queue 0x..: events=N/CAP exists=.. waiting-receivers=R first=0x..` reads the
  GCM handler queue (last created by context_allocate) through IDM with a try-lock on the
  queue mutex. "busy" means the queue mutex was held at that instant.

Interpretation (needs repeated coherent samples, not one line):

- The PR 20100 family is supported when the owner is vblank pkg=0xfed with a growing hold
  time, the VBlank phase cycles through event-full-wait/scheduler-check/retry, events is
  at CAP with zero waiting receivers, main_thread shows rsx-phase=context-lock, and the
  scheduler snapshot shows _gcm_intr_thread runnable but suspended.
- It is refuted by a different owner, a non-full queue, or a waiting receiver.

Host check: `python3 uwp/tests/check-stall-diagnostics.py` now also covers owner
publication/clear under a real mutex, the exempt null record and concurrent coherence.
The full sys_rsx.cpp was not compiled on the host (macOS Apple clang 14 lacks
`<source_location>`). The Xbox build compile and run are still unverified.

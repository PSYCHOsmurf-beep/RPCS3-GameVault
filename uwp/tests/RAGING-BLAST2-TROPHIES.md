# Raging Blast 2 boot freeze — verified findings, 19 November 2026

## Preserved evidence
Capture folder contains live Spartan RPCS3.log (10,435,303 bytes), RPCS3-after-quit.log (10,507,876 bytes), previous log, official app logs/configuration, package/process information, screenshot and original-byte hashes. All four Spartan boots are in ONE current log; separate gz logs returned404 and were not invented. First requests used an incorrectly escaped path, returned404, and were corrected before actual collection; no console writes. After-quit log ends with emulation stopped and suspension, not a fatal crash. Live capture is partial; after-quit capture contains termination.

## Package/game/sequence
Installed test0.1.35.11, source05d22c8667b9f24af59b3c3080deb240b7e32a87 over09d49cba5b743819632959c70aac8ff4fe606300. BLES00978 /APP_VER02.00 VERSION02.00, Auto frame limit, VSync Disabled. Four boot starts:0:00:58.474810,0:07:06.143416,0:10:02.653036,0:12:30.540325. Driver cache0KB on first and131KB on subsequent boots. 5GB allocation; stalled run memory~1.28GB. No evidence of reaching UWP memory limit.

Launch4:0:13:06.773801 registers trophy context1;0:13:07.042032 says initial callback will be called with trp_status3(already installed). Last completed frame0:13:07.043067:271ms,18draws,1submit,0newpipelines,0present wait,0GPU wait. Repeated snapshots keep DbPrize thread inside sceNpTrophyRegisterContext, main_thread at0x3be1a4(last sys_cond_signal), cri_dlg in sys_fs_read, SPUs largely waiting. RSX get==put==0x1c70. Termination at0:27:17.049145 aborts registration after818.831781s.

This localizes outstanding work to the registration call and application wait chain, NOT a demonstrated GPU fence hang. Last-HLE names do not identify the precise host instruction or establish a mutex cycle. Empty RSX FIFO can result from the game no longer issuing commands; a frozen spinner alone does not identify renderer ownership as the cause. sceNpTrophy logs only the initial callback ENTER, not return, TRP operations or later dispatch phases, so those are presently indistinguishable.

The first launch includes cellMsgDialogOpen2 and progress text Installing, then an abort of registration. Later installed-status boots omit the first installation UI but still register trophies. Do not delete trophy data or treat creation of the directory as proof all callback processing completed. Boot also logs CELL_EDEADLK/CELL_EPERM for mutex operations and savedata callback cancellation; these are leads, not proof they cause the persistent freeze. No explicit fatal/device-loss evidence explains the freeze.

## Source audit
Our installed .11 introduces exactly one emulator source-file change over its09d49cb base:RSXDisplayEngine.cpp queued-frame consumption moves after flip eligibility. No new VFS/sys_fs, memory, trophy/sysutil or scheduling changes were installed by our commit. The uncommitted .12 presentation candidate was never built/deployed; excluded as a cause.

User's actual source is emelin2004/rpcs3-xbox README compatibility table, based on Oct24–27 builds0.1.34.73 through0.1.35.7. It reports full-speed in-game play but no title serial, data revision or precise last-good commit. It is a compatibility report, not a same-console good/bad bisection. Resolved tags:v0.1.34.73=0686abb7edd25ae525f51d4edc16bb499086a341;v0.1.35.7=9b5ab467175b3bd9e041261ff05d802cf6e03e13. GitHub compare reports249 intervening commits to09d49cb, including substantial renderer refactoring. Detailed commit list in upstream-compare.json. Current sceNpTrophy.cpp and cellSysutil.cpp are byte-identical to original v0.1.35.7 files retrieved from GitHub. They are not a newly introduced trophy regression in our commit; caller/context/host scheduling can still differ.

## Primary sources
- https://github.com/emelin2004/rpcs3-xbox#what-works-october-2026 — source of the user's compatibility table.
- https://github.com/emelin2004/rpcs3-xbox/tree/v0.1.35.7 — comparison endpoint, not a proven good binary for this dump.
- https://github.com/RPCS3/rpcs3/issues/5088 — same trophy-then-loading symptom in BLUS30581 in2018; different version/platform.
- https://github.com/RPCS3/rpcs3/issues/5056 and https://github.com/RPCS3/rpcs3/pull/5096 — old status-sequence fix, already superseded by current implementation. Not a patch to transplant blindly.

## Next discriminating build, not a claimed fix
Isolated branch diagnostics/raging-blast2-trophies from installed05d22c8; source version0.1.35.13. Adds bounded phase markers to existing trophy registration: initial callback enter/return, manager lock, TRP header/install, TROPUSR load, dispatch start/end, first/last callback entry/return per status, and sysutil read count. Does NOT skip registration, change status sequence/return values/waits, modify settings or include pending presentation work. These records distinguish a stuck guest callback from TRP I/O or sysutil dispatch without another blind behavior change. Logging adds bounded overhead; real game testing is still required.

The exact responsible commit and targeted behavioral fix remain UNPROVEN. No claim of preserving/improving gameplay can be validated by a compile. Keep Bleach/J-Stars gains observational until controlled comparisons. If logging identifies callback completion starvation, inspect that exact dispatcher/caller path; if TRP I/O blocks, inspect that file/lock path; if neither, obtain host stacks or perform a same-settings code A/B before changing behavior.

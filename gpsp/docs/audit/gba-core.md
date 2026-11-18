# GBA core reliability checks — 2026-10-13

Malformed savestates now reject incompatible BSON field types and invalid timer,
DMA, display, EEPROM and backup-memory state before mutation. Lightweight rewind
restores retain their existing fast path. CPU interrupt, sound timer and DMA
boundary regressions are covered separately by the host registry.

`tools/tests/run_gba_state_checks.py --baseline` passed all 18 build/run checks
under AddressSanitizer and UndefinedBehaviorSanitizer. The original v4.0.0
accepts all 22 malformed typed fields and a zero timer reload; its malformed
EEPROM state reproduces a global-buffer overflow. The candidate rejects those
states. Candidate coverage passed both normal and REWIND/TIMER_LAZY builds,
including real state writers, valid round trips and preflight rejection.

Evidence: `audit/reliability/gba-core/state-results.json` and `gba-state.log`.
This is focused regression coverage, not a complete game compatibility corpus.

Today these checks are host suite `gba_state` (`tools/run_host_tests.py`), with
`test_gba_cpu_state.c` and `test_gba_sound_safety.c` added, in a plain and a
REWIND build. The script keeps no logs unless given `--out DIR`.

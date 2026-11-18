# Design summaries

These pages summarise design plans whose original drafts, reviews and run outputs were kept on a
development machine rather than in the repo. Code comments cite those plans by section number; each
page keeps the original numbers as headings, so a cite such as "PLAN.md §5" or "C-5 §2.1" still
lands on the right place. What each page says landed was checked against the tree.

| Page | What it covers | Cited in code as |
|---|---|---|
| [RESTRUCTURE.md](RESTRUCTURE.md) | The `psp/main_psp.c` restructure: why the one big frontend file became a shell plus `psp/fe/*.inc.c` fragments (still one translation unit), its gates, phases and proof tools T1..T16 | `PLAN.md §N`, `PLAN §N`, `PLAN-P-beta5 §N`, P0a..P7 |
| [EPOCH-C.md](EPOCH-C.md) | Epoch C: the one exit sequence (`fe/teardown.inc.c`), boot as a function (`fe/boot.inc.c`) and the session (`fe/session.inc.c`) | `PLAN-EPOCHC-beta5 §N`, C0..C3 step ids |
| [TIER-C.md](TIER-C.md) | Tier C code-health designs C-0..C-6: the frame loop as a function, the harness key table, layout pins, scheduled adapter ticks, telemetry structs, the core/frontend ABI header | `C-N.md §N`, `C5-0`, `C6-1`, `CRITIQUE C-1`, ... |
| [UI-MOCKUPS.md](UI-MOCKUPS.md) | UI mockup designs (menus, link mode, save states, the relay invite screen, splash and logo) | "the design", "the mockup" |

The proof tools themselves are documented in [tools/refactor/README.md](../../tools/refactor/README.md);
the clean baseline they were proven on is [tools/refactor/docs/baseline.md](../../tools/refactor/docs/baseline.md).

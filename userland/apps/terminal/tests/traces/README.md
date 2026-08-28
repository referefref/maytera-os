# Terminal damage-tracking traces

Byte streams captured from real programs on a real pty, plus the FRAME PLAN
that says how those bytes were split across the Terminal's event-loop ticks.
`../run_damage.sh` replays them through the production renderer.

Regenerate everything with `./capture.sh`. It needs `script(1)` (util-linux),
`vi`, `top` and `python3`, all of which the userland build container has.

| trace | what it is | why it is here |
|---|---|---|
| `claudeish` | `claudeish.py`, a spinner plus a pinned box-drawing status block redrawn at 10 Hz while text streams above it | The SHAPE of the reported fault: an agentic CLI over SSH. It is the only trace with non-ASCII cells, which matters because the non-ASCII path calls `win_draw_image()` and that syscall self-commits the whole window. |
| `vi` | a real `vi` driven through 30 page-downs, 30 page-ups and 40 single-line moves | A cursor-addressed full-screen editor, and the only trace with frames that change nothing at all. |
| `top` | a real `top -d 0.25 -n 20` | The honest WORST case: `top` genuinely rewrites most of the screen every refresh, so damage tracking can only do so much. Kept precisely so the reported ratio is not cherry-picked. |

`*.trace` is the raw output stream. `*.tm` is `script --log-timing`. `*.frames`
is one integer per event-loop tick: how many bytes arrived in that tick, at the
10 ms rate `term_layout_timeout_ms()` uses while a pane has a live child.
`mkframes.py` produces it. Ticks in which no byte arrived are dropped, so the
measurement is not flattered by counting frames that were always going to paint
nothing.

## Reading the output

`cells scanned` is what the pre-damage renderer painted for exactly this
workload (`term_rows * term_cols`, once per repaint). `cells painted` is what
the damage-tracked one painted. Both come from the same build over the same
bytes, because a TUI is not deterministic enough for two separate runs of two
separate binaries to be comparable.

Read the DISTRIBUTION, not the mean. A TUI has two populations of frame and the
average hides both: the frames that only move a spinner (which used to cost a
whole grid and now cost one or two cells) and the frames that scroll the
transcript (which still cost a viewport, because nothing here can make a scroll
cheap - see the note in `term_render.c`).

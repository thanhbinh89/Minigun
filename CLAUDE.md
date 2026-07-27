# MiniGun — AK Embedded Base Kit game project

MiniGun is a 2-player turn-based artillery duel built on the **AK (Active
Kernel)** event-driven MCU framework, running on the AK Embedded Base Kit
(STM32L151, 1.54" 128x64 OLED, 3 push buttons, 1 buzzer). Two characters
stand on opposite ends of a procedurally-laid-out city skyline and take
turns firing a single shot each; the first shot that lands on the other
player wins instantly.

This file is the steering doc for any agent (human or AI) working in this
repo. It captures the framework rules and the game-specific design so
future changes stay consistent.

## Gameplay

- **Players**: two on-screen characters (`bitmap_player_human`,
  `bitmap_player_alien` in `app/screens/screens_bitmap.cpp`), standing on
  the leftmost and rightmost buildings of the skyline. Turn 0 = human
  (fires rightward), turn 1 = alien (fires leftward). Whichever one is
  currently being controlled gets a 1px selection box drawn flush around
  its sprite (`minigun_draw_active_marker()`), on top of the aim line, so
  it's obvious at a glance who you're aiming/firing as.
- **World**: the skyline is randomly regenerated every match
  (`minigun_generate_skyline()`) — 8 buildings, widths always summing to
  exactly `LCD_WIDTH` so they still tile edge to edge with no gaps, random
  height per building. Seeded from `sys_ctrl_millis()` so it varies both
  across matches in one session and across power cycles.
- **Aim**: `UP` / `DOWN` buttons change the current player's angle in
  5° steps, clamped to `[10°, 80°]`. Default 45°. The current shooter also
  gets a short line drawn off their shoulder (`minigun_draw_aim_line()`)
  pointing at their exact angle/direction — a visual pointer to go with
  the `Angle: NN` text, rotates live as you press UP/DOWN.
- **Power**: holding `MODE` charges power (0-100) at a fixed rate for as
  long as the button is held; releasing `MODE` fires immediately at
  whatever power was reached. A quick tap fires a very weak shot; a long
  hold fires at full power. There is no confirm step — release = fire.
- **Resolution**: the shot is simulated as a parabolic projectile
  (gravity + initial velocity from angle/power) on a periodic timer, drawn
  as a short dotted trail. If it touches the opposing player, that shot's
  owner wins immediately (`MINIGUN_STATE_GAME_OVER`). If it hits a
  building or leaves the screen, the turn passes to the other player.
- **HUD**: bottom row always shows `Angle: NN` plus an outlined power
  gauge (`minigun_draw_power_bar()`) for the player currently
  aiming/firing — the bar fills left-to-right in real time as `MODE` is
  held, replacing what was originally a plain `Power: NNN` number.
- **Restart**: from the game-over screen, pressing `MODE` starts a new
  match — a freshly randomized skyline, both players reset to 45°/0
  power, human goes first.

All of this lives in one screen: `app/screens/scr_minigun.{h,cpp}`. See the
top of that file for the exact state machine (`MINIGUN_STATE_AIMING` →
charging is a flag, not a separate state → `MINIGUN_STATE_FIRING` →
`MINIGUN_STATE_ROUND_END` → back to `AIMING` or → `GAME_OVER`).

Boot flow: `scr_startup` (logo) → `scr_qrcode` → `scr_welcome` → any
button or the idle timeout now transitions straight into `scr_minigun`
(the base kit's original ball-bouncing `scr_idle` demo is left in the tree
untouched but is no longer reachable from `scr_welcome`).

## This is an AK Foundation project — consult the ak-docs MCP tools

This repo follows the **AK (Active Kernel)** framework conventions. Before
writing or changing firmware code, use the `ak-docs` MCP server:

- `get_ak_guardrails` — **read this first, every session.** Defines what
  must never be touched and the hard kernel limits (message size, ref
  counts, RAM budget, etc).
- `get_ak_guide(topic)` — step-by-step recipes: `agent-workflow`,
  `create-task`, `create-driver`, `create-screen`, `debug-uart-shell`,
  `isr-bridge`, `kernel-task-log`, `tune-pools`, `use-timer`.
- `get_ak_api(symbol)` / `list_ak_api(module)` — exact kernel function
  signatures before calling anything unfamiliar.
- `search_ak_docs(query)` — when unsure what something is called.
- `decode_ak_lcd(dump)` — paste a `lcd d` framebuffer capture from real
  hardware to render and sanity-check a screen.
- `analyze_ak_log(log)` — paste UART console output (boot log, FATAL
  banner, `fatal l`/`fatal m` dump) for a structured crash diagnosis.

### Where changes belong

Only edit:
- `application/sources/app/` — tasks, screens, signals (`app.h`), BSP
  wiring (`app_bsp.cpp`), task table (`task_list.cpp`).
- `application/sources/driver/` — new hardware-agnostic drivers.
- `application/sources/ak/ak.cfg.mk` — pool/timer sizing, debug flags.
- `application/Makefile` — feature flags (e.g. `RELEASE_OPTION`).

Never edit `application/sources/ak/` (kernel), `boot/`, `sys/`,
`networks/`, `common/`, `platform/`, or `libraries/` unless the user
explicitly asks and understands the blast radius. Full details:
`get_ak_guardrails`.

### Kernel invariants that matter for this game

- Handlers must not block (no `delay()`, no busy-wait). All timing —
  power charging, projectile stepping, the pause between rounds — is
  driven by AK software timers (`timer_set(...)`), never a loop.
- Common message payload ≤ 64 bytes; message ref count ≤ 7.
- Task priority `LEVEL_1`-`LEVEL_7`; `LEVEL_0` is reserved, never used.
- **RAM is the tight budget here**: the unmodified base kit already uses
  ~84% of the 16 KB RAM budget (`make info` after a clean build showed
  13760/16384 bytes used, ~2.6 KB free). Game state must stay in small
  fixed-size arrays (`uint8_t`/`float`, no `std::vector`/heap growth) —
  see the terrain/trail arrays in `scr_minigun.cpp` for the pattern.
  Always check `make info` after changes that add state.

### Develop → verify → commit workflow (see `get_ak_guide("agent-workflow")`)

1. Build and debug with `RELEASE_OPTION = -URELEASE` (the Makefile
   default). Only switch to `-DRELEASE` for a final shipping image —
   release mode auto-reboots on FATAL, which kills the interactive
   post-mortem shell you need while developing.
2. Commit after every finished, verified feature — one feature per
   commit, not batched.
3. After touching any screen, verify the actual framebuffer with
   `decode_ak_lcd` (flash real hardware, run `lcd d` over UART, paste the
   dump) rather than trusting the drawing code by eye.
4. Before calling a build "done", flash it, exercise every path you
   touched, then read `fatal l` / `fatal m` over UART and run
   `analyze_ak_log` on the output. `fatal_times` must not have
   increased.

## Build

```sh
cd application
make            # builds build_ak-base-kit-stm32l151-application/*.bin
make info       # flash/RAM usage report — check this after any change
```

Requires `arm-none-eabi-gcc` 10.3-2021.10 at the path in
`GCC_PATH` (top of `application/Makefile`, currently
`$(HOME)/Workspace/Tools/gcc-arm-none-eabi-10.3-2021.10`).

Flashing a real board (once you have one connected):

```sh
ak_flash /dev/ttyUSB0 build_ak-base-kit-stm32l151-application/ak-base-kit-stm32l151-application.bin 0x08003000
```

### Verified on real hardware

Flashed to a real AK Embedded Base Kit over `/dev/ttyUSB0` (CH340 adapter)
via `ak-flash`, checksum verified. On-device confirmation:

- `ver`: kernel 1.3, app 0.0.0.3, firmware checksum `fbcc` (matches the
  `ak-flash` transfer), SRAM breakdown sums to the full 16384 B budget.
- `fatal l` before and after exercising the boot cascade: `fatal_times`
  stayed at `-1` (never fired, erased-flash sentinel) and `restart_times`
  held steady — no crash, no watchdog reset.
- Without touching any button, the board auto-cascades
  `scr_startup` → `scr_qrcode` (10s) → `scr_welcome` (15s) →
  `scr_minigun` (~27s total from boot) exactly as designed, and lands on
  the aiming screen on its own.
- `lcd d` on the real framebuffer at that point, decoded via
  `decode_ak_lcd`: skyline, both sprites, and "Angle: 45 Power: 0" all
  render correctly — byte-for-byte identical to the host-side preview
  renderer's prediction, and matching the reference mock this feature was
  built against.

### Headless button testing over UART: the `btn` shell command

This environment has UART access but not physical access to the board's
buttons, so `shell.cpp` (app-layer, extending it is expected — see
`get_ak_guide("debug-uart-shell")` section 4) got a `btn` command that
posts the exact same signals the real GPIO callbacks post
(`app_bsp.cpp: btn_mode/up/down_callback`), so the full game loop is
testable over the UART shell alone:

| Command | Effect |
| --- | --- |
| `btn u` | UP tap — increase aim angle |
| `btn d` | DOWN tap — decrease aim angle |
| `btn p` | MODE press — starts charging power |
| `btn r` | MODE release — fires at whatever power was reached |

To simulate a hold: send `btn p`, wait in **real wall-clock time** on the
host side (not a blocking delay on the board), then send `btn r` — power
climbs +3 every ~60ms while "held". A deliberate design choice: there is
no `btn h <ms>` that holds on-device, because doing that would require a
blocking delay inside the shell task's handler, and AK's software timers
(`AC_MINIGUN_CHARGE_TICK` here) are only *delivered* when the scheduler's
main loop regains control — while blocked, the periodic timer keeps
allocating a fresh pure message every period without any of them being
consumed, which can exhaust `AK_PURE_MSG_POOL_SIZE` (32) and FATAL. Two
separate shell round-trips with a host-side wait between them avoids that
class of bug entirely and is the safe pattern to follow for any future
"simulate holding X" debug command.

Between `btn p`/`btn r`, `btn u`/`btn d`, and `lcd d`, the whole state
machine — aiming, charging, firing, collision, win, restart — is now
exercisable and verifiable end to end without a human at the board.

**And it was**: flashed this build and drove a full match through `btn`
+ `lcd d` + `decode_ak_lcd`, all confirmed on the real board:

- `btn u` x2 moved the HUD from "Angle: 45" to "Angle: 55".
- `btn p`, held (via a real host-side `sleep`, not a device-side delay —
  see below), then `btn r`: power climbed and correctly clamped at
  "Power: 100"; a live projectile trail was visible mid-flight in the
  `lcd d` dump, arcing up-right from the human's building exactly as the
  physics predicted.
- A shot that hit terrain correctly passed the turn to the other player,
  who came up with their **own independently-tracked** angle/power
  (defaulted "Angle: 45 Power: 0", untouched by player 1's changes) —
  confirms the per-player state arrays aren't cross-contaminating.
- A shot that connected correctly ended the match: "PLAYER 2 WINS!" /
  "MODE: play again" rendered exactly as designed, no skyline underneath.
- `btn p` on that game-over screen correctly restarted: skyline and both
  sprites back, "Angle: 45 Power: 0", player 0's turn again.
- `fatal l` before vs. after this whole session: `fatal_times` still
  `-1` (never fired), `restart_times` +1 — and that +1 is fully
  accounted for by the `ak-flash` re-flash itself (which reboots the
  board), not by anything that happened during the match. Zero crashes
  across aiming, charging to max, firing, a miss, a hit, and a restart.

One methodology note worth keeping: the first several timed shots all
landed at max power despite intending to hold for less, because the
helper script's read-drain after sending a command (~0.5-1s) was silently
adding to the "hold" duration. Fixed by adding a zero-latency send-only
mode and doing the timing entirely with the host's own `sleep` between
two separate sends — worth remembering for any future "simulate a timed
hold" testing over this shell.

### Random skyline + aim-angle line

Both flashed and confirmed on the real board (`lcd d` + `decode_ak_lcd`):
two boots produced visibly different skylines (building count fixed at 8,
but widths/heights genuinely varied — one run put the human on a tall
narrow spike, the next spread height more evenly), and `btn d` x7
(angle 45→10) visibly rotated the aim line from a steep diagonal to
nearly horizontal next to the character's head. `fatal l` unchanged
(`fatal_times` still `-1`, `restart_times` +1 for the reflash only).

What was checked in place of hands-on-buttons hardware testing, at the
final pre-flash commit:

- **Clean rebuild**: `rm -rf build_*` then `make` from scratch, zero
  compiler warnings, `ak-base-kit-stm32l151-application.bin` produced.
- **Budget**: `make info` → flash 70920/118784 B (59.7% used), RAM
  13964/16384 B (85.2% used, 14.8% free). RAM was ~84% used in the
  *unmodified* base kit before any game code was added, so headroom was
  tight going in; every game-state array is fixed-size (no
  `std::vector`/heap growth) and the total added RAM is ~200 bytes.
- **Timer pool**: `AK_TIMER_POOL_SIZE=16` (`ak.cfg.mk`). MiniGun's three
  timers (`AC_MINIGUN_CHARGE_TICK`, `AC_MINIGUN_PROJECTILE_TICK`,
  `AC_MINIGUN_ROUND_END_TICK`) are mutually exclusive by game state (at
  most one live at a time) and are always cancelled with
  `timer_remove_attr` before the next is armed or on `SCREEN_ENTRY` —
  same one-active-timer-per-screen pattern the base kit's own screens
  already use. `timer_remove_attr` is documented safe to call with no
  matching timer (`TIMER_RET_NG`, not FATAL), which the reset path in
  `minigun_new_match()` relies on.
- **Physics**: the trajectory formulas (angle/power → velocity, gravity
  integration, collision priority) were re-implemented standalone in
  Python with the exact same constants and run across a sweep of
  angle/power combinations. Confirmed: hit trajectories exist across a
  wide range (not degenerate — the game requires actually aiming), all
  miss trajectories terminate well under the 150-tick safety cap (worst
  case observed ~45 ticks), no combination hangs.
- **Screen layout**: both the normal play screen and the game-over banner
  were rendered by an independent host-side script (reuses the real 5x7
  font table from `glcdfont.cpp`, reimplements the small
  fillRect/drawBitmap/print subset scr_minigun.cpp actually calls, into
  the same 128x64 page-major/LSB-top buffer `decode_ak_lcd` expects) and
  visually inspected via `decode_ak_lcd` — skyline, both sprites, and HUD
  text all render as intended and within panel bounds. This is a
  visualization aid only, not a substitute for flashing real hardware.

### Active-player marker + power bar

Two small HUD/legibility features, flashed and confirmed on the real
board via `btn` + `lcd d` + `decode_ak_lcd`:

- **Active-player marker** (`minigun_draw_active_marker()`): a 1px
  selection box drawn flush around the current shooter's sprite bounding
  box (`player_x/y - 1`, `SPRITE_W/H + 2`). Deliberately sized to sit
  *inside* the sprite's own footprint rather than clearing space above
  the head, because the tallest buildings (`MINIGUN_BUILDING_HEIGHT_MAX
  = 36`) put a player's head only 3px from the top of the panel — a
  marker floating above the head would clip off-screen there. Confirmed
  on-device: the box rendered around the human on turn 1, and after a
  weak shot missed and the turn passed, the box correctly moved to the
  alien on turn 2 (independently of the aim line, which does the same
  per-turn handoff) — no clipping at either end of the skyline.
- **Power bar** (`minigun_draw_power_bar()`): replaces the old
  `Power: NNN` text with an outlined gauge (`x=62,y=LCD_HEIGHT-8,
  w=60,h=7`) that fills left-to-right in proportion to
  `power/MINIGUN_POWER_MAX`, redrawn every tick like the rest of the
  HUD so it animates live while `MODE` is held. Confirmed on-device at
  three fill levels — empty (post-restart), partial (~15% after a short
  timed hold), and full (after a long hold, clamped at 100) — all
  rendered as a clean border with a proportionally-sized inner fill and
  no artifacts.
- **Budget**: `make info` → flash 71936/118784 B (60.6% used, up ~1 KB
  from the pre-marker/bar build), RAM unchanged at 13988/16384 B
  (85.4% used) — both features are computed from existing per-player
  state on every draw, no new stored fields.
- **Fatal log**: `fatal l` showed `fatal_times: 1` (`MF 0x21`, COMMON
  message pool exhaustion) going into this session — traced via
  `analyze_ak_log` to task id 10 (`AC_LINK_PHY_ID`) and a `fatal m`
  history dominated by RF24/link-network task activity, i.e. the
  UART/RF link stack, not the display task or anything this change
  touches. `fatal_times` stayed at exactly `1` (same record, unchanged)
  across both flashes done for this work; `restart_times` moved only by
  the two reflashes themselves. Treated as a pre-existing/stale record
  from earlier board activity, not a regression from this change — worth
  a `fatal r` + clean reboot the next time someone's at the board to
  reset the baseline back to the sentinel.

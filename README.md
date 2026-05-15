# D2R Damage Numbers

D2R Damage Numbers is a Windows overlay for Diablo II: Resurrected that displays floating damage numbers above monsters and an optional DPS readout. It watches monster HP drops from the running game process, projects hits back onto the game window, and renders a transparent, click-through overlay on top of D2R.

The project is intended for local experimentation and modding research. It reads game process memory, so use it at your own risk.

Compatibility note: this has only been tested in single player with the Steam version of Diablo II: Resurrected - Infernal Edition, `D2R.exe` file/product version `3.0.92198`. Other launchers, editions, regions, multiplayer modes, or game builds may use different memory layouts and may not work without adjustments. Multiplayer use is at the user's own risk.

## Features

- Floating damage numbers for real monster HP drops.
- Optional DPS meter with configurable screen position and rolling window.
- Coalescing for rapid small ticks, useful for poison, flame, and other repeated damage.
- Tick-pop animation for coalesced damage so each small tick still gets visual feedback.
- Stack lanes so separate hits on one target do not sit directly on top of each other.
- Target following using world-position projection when enough position data is available.
- Optional test mode with hotkeys for synthetic damage numbers.
- Tray menu for common toggles, config reload, log opening, and test burst spawning.
- Debug tools for position candidate research and hit-recorder snapshots.

## Requirements

- Windows.
- Diablo II: Resurrected running in a normal window or borderless window.
- Visual Studio 2022 with the C++ desktop workload if building from source.
- The shipped font file next to the executable: `diablo4.ttf`.

## Running

Build `D2RDamageNumbers.sln` as `x64 Release`, then run:

```text
x64\Release\D2RDamageNumbers.exe
```

Keep `D2RDamageNumbers.ini` and `diablo4.ttf` next to the executable. The app loads the INI from the executable directory, so the normal runtime config is:

```text
x64\Release\D2RDamageNumbers.ini
```

After editing the INI, use the tray menu's `Reload Config` command. You do not need to restart Diablo II: Resurrected or restart this overlay; config changes take effect immediately after reload.

Position learning note: the overlay may require you to hover over roughly 3-7 monsters before damage numbers position correctly. This gives the world-position projection enough samples for your current camera/resolution.

## Tray Menu

Right-click the tray icon to open the menu.

- `Test Mode`: toggles synthetic test hotkeys and saves `[Test] Enabled`.
- `Show Debug Status`: toggles the debug text overlay and saves `[Debug] ShowStatus`.
- `Show Position Candidates`: toggles position markers and saves `[Debug] ShowPositionCandidates`.
- `Write Log`: toggles diagnostic logging and saves `[Log] Enabled`.
- `Reload Config`: reloads the INI immediately, restarts the frame timer, and trims active numbers if needed.
- `Open Config`: opens the active INI file.
- `Open Log`: opens `D2RDamageNumbers.log`.
- `Spawn Test Burst`: spawns a synthetic burst at the game-window center.
- `Exit`: closes the overlay.

Double-clicking the tray icon toggles the debug status overlay.

## Configuration

All options live in `D2RDamageNumbers.ini`. Boolean values use `0` or `1`. Colors accept `#RRGGBB` or `R,G,B`.

### Overlay

| Key | Description |
| --- | --- |
| `FontFile` | Optional private font file next to the INI/EXE. Empty means use an installed Windows font. |
| `FontFace` | Font family name used by GDI. This must match the font's internal family name. |
| `FontSize` | Base size for normal damage numbers. |
| `CritFontSize` | Base size for prominent damage numbers. |
| `FontWeight` | Normal font weight, from `100` to `1000`. |
| `CritFontWeight` | Prominent hit font weight, from `100` to `1000`. |
| `OutlineThickness` | Damage text outline thickness. Set `0` to disable. |
| `ShadowOffsetX`, `ShadowOffsetY` | Damage text shadow offset. Set both to `0` to disable shadow. |
| `MaxActiveNumbers` | Maximum floating numbers kept alive at once. |
| `ShowDpsNumber` | Shows or hides the fixed DPS meter. |
| `DpsNumberXPercent` | DPS meter horizontal position. `0` is left, `100` is right. |
| `DpsNumberYPercent` | DPS meter vertical position. `0` is top, `100` is bottom. |
| `DpsRollingWindowSeconds` | DPS window length. DPS is damage inside this window divided by this value. |
| `CoalesceSmallDamage` | Merges repeated small normal hits on the same target into one rolling total. |
| `CoalesceMaxDamage` | Maximum hit amount eligible for coalescing. |
| `CoalesceWindowMs` | How long a target's rolling total can wait for another tick. |
| `CoalesceRefreshLifetime` | Extra lifetime added when a coalesced total refreshes. |
| `CoalescePulseScale` | Scale pulse applied to the rolling total when it updates. |
| `CoalescePulseSeconds` | Duration of the rolling-total pulse. |
| `CoalesceTickPop` | Shows a small `+tick` number flying into the rolling total. |
| `CoalesceTickPopSeconds` | Duration of the `+tick` merge animation. |
| `CoalesceTickPopScale` | Size of the `+tick` text relative to normal damage text. |
| `CoalesceTickPopDistance` | Start distance for the `+tick` merge animation. |
| `CoalesceTickPopMergeOffsetY` | Vertical offset of the merge point relative to the rolling total. Negative is above. |
| `UseStackLanes` | Spreads separate hits on one target across lanes instead of piling them up. |
| `StackLanes` | Number of lanes available for stacked hits. |
| `StackLaneWidth` | Horizontal spacing between stack lanes. |
| `StackVerticalStep` | Vertical step used when lanes are reused. |
| `StackReuseSeconds` | How long a recent number participates in stack-lane placement. |
| `StackMaxYOffset` | Maximum vertical offset added by stack lanes. |
| `RenderFps` | Overlay redraw target. Match the game FPS for smoother motion. |
| `LifetimeSeconds` | Normal hit lifetime in seconds. |
| `CritLifetimeSeconds` | Prominent hit lifetime in seconds. |
| `FadeStart` | Fraction of lifetime before fade starts. |
| `PopStartScale` | Initial spawn scale. |
| `PopOvershootScale` | Peak spawn pop scale. |
| `PopInSeconds` | Time spent scaling from start to overshoot. |
| `PopSettleSeconds` | Time spent settling from overshoot to normal scale. |
| `FloatSpeed` | Upward float speed in pixels per second. |
| `HorizontalDrift` | Random sideways drift after spawn. |
| `SpawnYOffset` | Vertical spawn offset from the target anchor. Negative is higher. |
| `FollowResponse` | How quickly active numbers catch up to moving targets/camera changes. |
| `FollowSnapDistance` | Large projection changes are treated as outliers/speed-limited. |
| `FollowMaxSpeed` | Maximum target-follow movement speed in pixels per second. |

### Test

| Key | Description |
| --- | --- |
| `Enabled` | Enables synthetic test hotkeys. Does not read game memory. |
| `NormalHotkey` | Spawns a normal test hit at the cursor. |
| `CritHotkey` | Spawns a critical test hit at the cursor. |
| `BurstHotkey` | Spawns a mixed burst at the cursor. |
| `BurstCount` | Number of hits in a synthetic burst. |
| `BurstRadius` | Random radius for synthetic burst placement. |

Hotkeys can be function keys like `F8`, hex virtual-key values like `0x70`, or a single letter/number.

### Log

| Key | Description |
| --- | --- |
| `Enabled` | Writes diagnostic and hit-recorder entries to `D2RDamageNumbers.log`. |

### Memory

| Key | Description |
| --- | --- |
| `PollSeconds` | How often memory is sampled. Lower values catch shorter-lived changes but cost more CPU. |
| `TrackAllMonsters` | `1` tracks HP drops for all readable monsters. `0` uses hovered-monster tracking. |
| `UseWorldPositions` | Reads unit world/path coordinates and projects damage near the damaged unit. |
| `FollowDamageTargets` | Keeps active numbers attached to targets as camera/player movement changes projection. |
| `UseScreenAnchorCandidates` | Experimental learner for actual screen coordinate fields in target structures. |
| `UseWorldCoordCandidates` | Experimental learner for alternative memory-field world coordinates. |
| `ScreenAnchorMinSamples` | Minimum samples before screen-anchor candidates can be trusted. |
| `ScreenAnchorMinDistinctUnits` | Minimum distinct units needed for screen-anchor learning. |
| `ScreenAnchorMaxCursorDistance` | Maximum cursor distance allowed for a candidate sample. |
| `ScreenAnchorMaxAverageError` | Maximum average error for a screen-anchor model. |
| `ScreenAnchorMinMovementSpan` | Minimum movement span before a screen-anchor model is accepted. |
| `LearnWorldProjection` | Learns projection from hover samples for the current camera/resolution. |
| `WorldProjectionMinSamples` | Minimum samples before learned projection can be used. |
| `WorldCoordMinDistinctUnits` | Minimum distinct units needed for world-coordinate candidate learning. |
| `WorldCoordMaxRmsError` | Maximum RMS error for a world-coordinate model. |
| `WorldCoordMinMovementSpan` | Minimum movement span before accepting a world-coordinate model. |
| `WorldCoordScreenOffsetX`, `WorldCoordScreenOffsetY` | Manual screen offset for world-coordinate projection. |
| `WorldAnchorX`, `WorldAnchorY` | Fallback projection anchor as fractions of the game window. |
| `WorldTileWidth`, `WorldTileHeight` | Fallback isometric world-tile projection scale. |
| `PositionProbeHotkey` | Writes a position probe while hovering an enemy. |
| `HitRecorder` | Enables rolling memory snapshots around real hits. |
| `MarkerHotkey` | Writes a marker around the latest hit for research. |

### Debug

| Key | Description |
| --- | --- |
| `ShowStatus` | Shows debug status text on the overlay. |
| `ShowPositionCandidates` | Shows lightweight hover/position markers. |
| `PositionCandidateHotkey` | Toggles position candidates while running. |

### Colors

| Key | Description |
| --- | --- |
| `Normal` | Normal damage number color. |
| `Critical` | Critical damage number color. |
| `Outline` | Damage text outline color. |
| `Shadow` | Damage text shadow color. |

## Debugging

Run once with memory diagnostics:

```text
D2RDamageNumbers.exe --scan-memory
```

Enable `[Debug] ShowStatus=1` and `[Log] Enabled=1` for live pattern, memory, HP, tracking, and projection status.

## Project Layout

- `D2RDamageNumbers.cpp`: Win32 app shell, overlay window, main frame loop, tray/memory/render integration.
- `OverlayConfig.*`: INI loading and config defaults.
- `OverlayRenderer.*`: GDI layered-window rendering.
- `DamageNumberSystem.*`: damage number spawning, coalescing, stack lanes, and target follow.
- `DpsTracker.*`: rolling DPS calculation.
- `MemoryScanner.*`: D2R process discovery, pattern resolution, HP tracking, projection research tools.
- `D2RMemoryLayout.h`: offsets and memory-layout constants that are likely to need updates after game patches.
- `FrameTimer.*`: high-frequency render tick scheduling.
- `TrayController.*`: tray icon, tray menu, and saved toggles.

## License

Code is licensed under the MIT License. See `LICENSE`.

The bundled `diablo4.ttf` font is PT Serif and is licensed separately under the SIL Open Font License 1.1. See `OFL.txt`.

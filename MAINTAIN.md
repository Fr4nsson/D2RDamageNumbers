# Maintaining D2R Damage Numbers

This file is for maintainers who need to repair the overlay after a Diablo II: Resurrected update. Most breakage comes from memory signatures or structure offsets changing.

## Fast Triage After a Game Update

1. Build `x64 Release`.
2. Start D2R and load into a game.
3. Run:

   ```text
   x64\Release\D2RDamageNumbers.exe --scan-memory
   ```

4. Also test the normal overlay with:

   ```ini
   [Debug]
   ShowStatus=1

   [Log]
   Enabled=1
   ```

5. Check the debug overlay or `D2RDamageNumbers.log` for:

   - `patterns not found`: update signatures in `MemoryScanner.cpp`.
   - `hover pattern ready; game pattern missing`: current-game signature changed.
   - D2R found but memory disconnected: process/module discovery or access changed.
   - Patterns ready but HP missing/wrong: unit/stat offsets likely changed.
   - Damage appears but anchors are wrong: path/world coordinate offsets or projection tuning changed.

## Files That Usually Need Updates

### `D2RMemoryLayout.h`

Update this first when structures move. It intentionally centralizes known offsets so rendering and app-shell code do not need to change.

Common constants to verify:

- `D2_UNIT_OFFSET_TYPE`
- `D2_UNIT_OFFSET_ID`
- `D2_UNIT_OFFSET_PATH`
- `D2_UNIT_OFFSET_STATS`
- `D2_UNIT_OFFSET_NEXT`
- `D2_SERVER_UNIT_OFFSET_NEXT`
- `D2_PATH_OFFSET_X`
- `D2_PATH_OFFSET_Y`
- `D2_STATLIST_BASE_ARRAY`
- `D2_STATLIST_EXTRA_ARRAY`
- `D2_GAME_UNIT_TABLE_*`

If HP reads are broken, focus on unit stats and stat-list offsets. If anchors are broken, focus on path/world coordinate offsets.

### `MemoryScanner.cpp`

Update this when pattern scanning fails or globals move.

The main pattern areas are:

- `mouseHoverPatterns`
- `mouseHoverModulePatterns`
- `currentGamePatterns`
- `unitHashPatterns`
- `BuildPatternDiagnostics`

The scanner supports IDA-style signatures with wildcards. Keep multiple candidate patterns when possible, and give each candidate a useful name. The debug overlay and log display the winning pattern name and match count, which makes future updates much easier.

### `OverlayConfig.h` and `D2RDamageNumbers.ini`

Update these together when adding config keys. Runtime defaults live in `OverlayConfig.h`; user-facing documented defaults live in the INI files.

Keep these three INIs aligned unless you intentionally need a different debug default:

- `D2RDamageNumbers.ini`
- `x64\Release\D2RDamageNumbers.ini`
- `x64\Debug\D2RDamageNumbers.ini`

## Pattern Update Checklist

1. Run `--scan-memory` before changing anything and save the output.
2. Identify which pattern group failed: hover, current game, or unit hash.
3. Update the relevant signature candidates in `MemoryScanner.cpp`.
4. Add or update diagnostics in `BuildPatternDiagnostics` so future failures show useful match counts.
5. Rebuild Release.
6. Run `--scan-memory` again.
7. Confirm each required pattern has exactly one strong match or a validated best match.
8. Test in a real game with `[Debug] ShowStatus=1`.

## Offset Update Checklist

1. Confirm signatures are resolving first. Bad patterns can look like bad offsets.
2. Use `[Log] Enabled=1` so memory status and hit-recorder output are written.
3. Verify unit type/id/next pointers by checking monster tracking counts.
4. Verify HP/max HP reads on a hovered monster.
5. Verify world/path coordinates by hovering a few monsters and watching projection status.
6. Update `D2RMemoryLayout.h`.
7. Rebuild and test hovered tracking with `TrackAllMonsters=0`.
8. Test full monster tracking with `TrackAllMonsters=1`.

## Position and Projection Repair

When damage numbers appear but sit in the wrong place:

- Set `[Debug] ShowStatus=1`.
- Set `[Debug] ShowPositionCandidates=1`.
- Hover monsters in different screen positions.
- Use `PositionProbeHotkey` while hovering a monster and placing the cursor where the lock should be.
- Check `World pos`, `World coord`, and `Screen lock` lines in the debug overlay.

Relevant config:

- `UseWorldPositions`
- `FollowDamageTargets`
- `LearnWorldProjection`
- `WorldProjectionMinSamples`
- `WorldAnchorX`
- `WorldAnchorY`
- `WorldTileWidth`
- `WorldTileHeight`
- `UseScreenAnchorCandidates`
- `UseWorldCoordCandidates`
- `WorldCoordScreenOffsetX`
- `WorldCoordScreenOffsetY`

Prefer fixing real offsets in `D2RMemoryLayout.h` before relying on experimental candidate learners.

## Hit Recorder Workflow

The hit recorder exists for researching fields that change around a hit, especially future work like real crit/deadly-strike detection.

1. Set:

   ```ini
   [Memory]
   HitRecorder=1

   [Log]
   Enabled=1
   ```

2. Hit a monster.
3. Press `MarkerHotkey` soon after a hit you care about.
4. Inspect `D2RDamageNumbers.log`.

The recorder keeps snapshots around recent hits and logs nearby memory/stat differences. Keep snapshot sizes and limits in `D2RMemoryLayout.h` unless the structures grow.

## Verification Before Release

Always verify:

- Test mode still spawns normal, critical, and burst numbers.
- Real hovered-monster damage still appears with `TrackAllMonsters=0`.
- Real all-monster damage still appears with `TrackAllMonsters=1`.
- Coalescing still works for repeated small ticks.
- Stack lanes still spread separate hits.
- DPS reads correctly with default `DpsRollingWindowSeconds=5`.
- The tray menu can reload config and toggle log/debug options.

Build both configurations:

```text
MSBuild D2RDamageNumbers.sln /p:Configuration=Release /p:Platform=x64 /m
MSBuild D2RDamageNumbers.sln /p:Configuration=Debug /p:Platform=x64 /m
```

## Design Notes

- Keep offsets and D2R memory constants in `D2RMemoryLayout.h`.
- Keep pattern scanning and memory reads in `MemoryScanner.cpp`.
- Keep visual behavior in `DamageNumberSystem.cpp` and `OverlayRenderer.cpp`.
- Keep INI parsing in `OverlayConfig.cpp`.
- Avoid moving game-update fixes into `D2RDamageNumbers.cpp`; that file should stay mostly app orchestration.

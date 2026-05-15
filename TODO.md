# TODO / Roadmap

This file tracks larger follow-up work that is not required for the current overlay to run, but would make it more accurate and maintainable.

## Real Critical Hit Detection

Current state:

- Real damage is detected from monster HP drops.
- Real critical/deadly-strike state is not confirmed.
- Critical damage coloring currently exists for synthetic test events only.

Goal:

- Detect whether a real hit was critical/deadly strike and color/scale that damage number correctly.

Possible work:

- Use `HitRecorder=1` and `MarkerHotkey` to capture memory around hits that are known or suspected crits.
- Compare player, target, stat-list, and hit-context snapshots around normal hits vs critical hits.
- Investigate D2 stats related to crit/deadly strike in `D2RMemoryLayout.h`.
- Find a reliable per-hit flag, animation state, combat event, or stat delta that distinguishes crits from normal hits.
- Keep fallback behavior as normal damage if crit confidence is low.
- Add config only after the signal is reliable, for example `ShowCriticalHits=1`.

Verification:

- Test with a character/build that has predictable critical/deadly strike chance.
- Log repeated hits against the same monster type.
- Confirm crit coloring does not trigger on normal hits.
- Confirm normal hits still display correctly when no crit signal is found.

## Replace World Projection With Real Target Lock

Current state:

- Damage number placement mainly uses world/path coordinates and learned projection.
- Users may need to hover roughly 3-7 monsters before projection becomes stable.
- Projection is resolution/camera dependent and can drift or be wrong when the learned model is poor.

Goal:

- Replace learned world projection with a real lock point from game memory, ideally an actual target screen coordinate or stable UI/combat anchor.

Possible work:

- Continue researching screen-coordinate fields with `UseScreenAnchorCandidates=1`.
- Use `PositionProbeHotkey` while hovering enemies to compare cursor position with candidate lock fields.
- Prefer a real screen-space target anchor over inferred isometric world projection.
- Validate any candidate across different monster types, positions, resolutions, and camera movement.
- Keep world projection as a fallback until the real lock is reliable.
- Once real lock is reliable, simplify or disable projection learning by default.

Verification:

- Damage numbers should appear centered on the damaged monster without requiring hover training.
- Active numbers should stay attached during camera/player movement.
- No jumps to screen corners or far-away positions.
- Works with `TrackAllMonsters=1`, not only hovered monsters.
- Works after changing resolution/window size.

## Cleanup After These Are Solved

- Update `README.md` and `MAINTAIN.md` with the new behavior.
- Remove or demote obsolete experimental projection settings if they are no longer needed.
- Keep game-update-sensitive offsets in `D2RMemoryLayout.h`.
- Keep memory scanning/research code in `MemoryScanner.cpp`.

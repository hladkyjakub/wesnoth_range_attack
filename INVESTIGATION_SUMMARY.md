# Complete Investigation Summary

## Original Questions

1. **Previous Question:** When reachmap is updated, the `process_reachmap_changes()` doesn't immediately return - this is true for all cases, except for the case when unit performs the move&attack - the reachmap gets wiped, but `process_reachmap_changes()` does return right at the start - so it has to be done by some other code - could you identify the code?

2. **Follow-up Question:** Where is `gui().unhighlight_reach()` defined? It does set the `reach_map_changed_` to true, only place that sets it to false is `process_reachmap_changes()`. So that would imply that `process_reachmap_changes()` is called two times in a row, and in between is nothing that would set `reach_map_changed_` to true executed.

## Answers

### Question 1: What Code Causes Early Return?

**Answer:** `controller_base::play_slice()` in `src/controller_base.cpp:408`

This function is called repeatedly during unit movement animations and triggers a display update chain that processes reach map changes mid-animation, before the attack code runs.

**Documentation:** See `REACHMAP_ANALYSIS.md` for complete call chain analysis.

### Question 2: Where is unhighlight_reach() and Why No Double Processing?

**Answer:** `game_display::unhighlight_reach()` in `src/game_display.cpp:554`

The key insight is that `unhighlight_reach()` **only sets `reach_map_changed_ = true` if reach_map_ is NOT empty**:

```cpp
bool game_display::unhighlight_reach()
{
    if(!reach_map_.empty()) {
        reach_map_.clear();
        reach_map_changed_ = true;
        return true;
    } else {
        return false;  // No flag change when already empty!
    }
}
```

**Why there's no double processing:**

1. First call (move, line 1206): reach_map not empty → clears it, sets flag to true
2. Animation processing: `process_reachmap_changes()` runs, sets flag to false
3. Second call (attack, line 1430): reach_map ALREADY empty → does NOT set flag to true
4. Result: No second processing needed, flag stays false

This is **not** a case of `process_reachmap_changes()` being called twice with nothing in between. The second `unhighlight_reach()` is essentially a no-op because the reach_map was already cleared.

**Documentation:** See `UNHIGHLIGHT_REACH_EXPLANATION.md` for detailed explanation with visual diagrams.

## Complete Documentation Set

1. **README.md** - This summary document
2. **QUICK_ANSWER.md** - Quick reference with key points
3. **REACHMAP_ANALYSIS.md** - Complete call chain analysis for Question 1
4. **UNHIGHLIGHT_REACH_EXPLANATION.md** - Detailed explanation for Question 2
5. **call_chain_diagram.txt** - ASCII flow diagram

## Key Insights

1. **Display updates happen during animations** via the event loop (`controller_base::play_slice()` → `events::draw()` → display update chain)

2. **`unhighlight_reach()` is idempotent** - calling it multiple times is safe because it only sets the change flag when there's actually something to clear

3. **The design prevents redundant work** - once reach_map is empty, subsequent calls to `unhighlight_reach()` don't trigger display invalidations

## Related Code Locations

- **unhighlight_reach() definition:** `src/game_display.cpp:554-564`
- **unhighlight_reach() declaration:** `src/game_display.hpp:104`
- **process_reachmap_changes():** `src/display.cpp:3307-3344`
- **play_slice():** `src/controller_base.cpp:408`
- **Move initiation:** `src/mouse_events.cpp:1206`
- **Attack initiation:** `src/mouse_events.cpp:1430`
- **Animation loop:** `src/units/animation.cpp:1413,1420`

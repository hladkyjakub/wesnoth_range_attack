# Quick Answer: Reachmap Early Return During Move&Attack

## Question
When a unit performs move&attack, the reachmap gets wiped, but `process_reachmap_changes()` returns immediately. What code is responsible for this?

## Answer
**`controller_base::play_slice()`** in `src/controller_base.cpp:408`

## Why This Happens

1. **During Move** (mouse_events.cpp:1206): `gui().unhighlight_reach()` clears the reach map and sets `reach_map_changed_ = true`

2. **During Animation** (animation.cpp:1413,1420): The animation loop repeatedly calls `controller_base::play_slice()`

3. **Inside play_slice** (controller_base.cpp:418): Calls `events::draw()`

4. **Draw Chain**: 
   ```
   events::draw() 
   → draw_manager::sparkle() 
   → draw_manager::update() 
   → game_display::update() 
   → process_reachmap_changes()
   ```

5. **Processing Changes** (display.cpp:3343): `process_reachmap_changes()` processes the changes and sets `reach_map_changed_ = false`

6. **Before Attack** (mouse_events.cpp:1430): `gui().unhighlight_reach()` is called again
   - **IMPORTANT:** At this point, `reach_map_` is already empty (from step 1)
   - The `if(!reach_map_.empty())` check fails
   - Does NOT set `reach_map_changed_ = true`
   - This is why no second processing occurs!

7. **Early Return** (display.cpp:3309): `reach_map_changed_` remains `false`, so function returns immediately

## Key Insights

1. **Display updates happen during animations**, not just at discrete game state transitions. This is why reach map changes are processed during the move animation, before the attack code runs.

2. **`unhighlight_reach()` only sets the flag if reach_map is not empty**. This prevents redundant processing - the second call to `unhighlight_reach()` is a no-op because the reach_map was already cleared by the first call.

## Files to Check
- `src/controller_base.cpp:408` - The key function
- `src/units/animation.cpp:1413,1420` - Where it's called during animations
- `src/display.cpp:3307-3344` - Where process_reachmap_changes is defined
- `src/game_display.cpp:554-564` - Where unhighlight_reach is defined
- `src/mouse_events.cpp:1199-1225` - Move initiation
- `src/mouse_events.cpp:1430` - Attack initiation

See `REACHMAP_ANALYSIS.md` for complete detailed analysis with full call chain.
See `UNHIGHLIGHT_REACH_EXPLANATION.md` for detailed explanation of the unhighlight_reach() behavior.

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

7. **Early Return** (display.cpp:3309): Since `reach_map_changed_` is already `false`, the function returns immediately

## Key Insight
Display updates happen **during animations**, not just at discrete game state transitions. This is why reach map changes are processed during the move animation, before the attack code runs.

## Files to Check
- `src/controller_base.cpp:408` - The key function
- `src/units/animation.cpp:1413,1420` - Where it's called during animations
- `src/display.cpp:3307-3344` - Where process_reachmap_changes is defined
- `src/mouse_events.cpp:1199-1225` - Move initiation
- `src/mouse_events.cpp:1430` - Attack initiation

See `REACHMAP_ANALYSIS.md` for complete detailed analysis with full call chain.

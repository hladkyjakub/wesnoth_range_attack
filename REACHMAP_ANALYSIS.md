# Reachmap Changes Processing Analysis

## Problem Statement

When `reachmap` is updated, `process_reachmap_changes()` normally processes the changes and doesn't immediately return. However, there is one exception: when a unit performs a **move&attack** action, the reachmap gets wiped, but `process_reachmap_changes()` returns immediately at the start (at line 3309 in `display.cpp` due to the early return `if (!reach_map_changed_) return;`).

This document identifies the code responsible for this behavior.

## Key Finding

The code responsible for this early return behavior is **`controller_base::play_slice()`** which is called repeatedly during unit movement animations. This triggers a chain of function calls that ultimately calls `process_reachmap_changes()` during the animation, processing the reach map changes before the attack occurs.

## Complete Call Chain

### 1. Move Initiation
**File:** `src/mouse_events.cpp`
**Function:** `mouse_handler::move_unit_along_current_route()` (line 1199)
```cpp
// Line 1206: Clear reach map during move
gui().unhighlight_reach();
```
This sets `reach_map_changed_ = true` and clears the `reach_map_`.

### 2. Animation Loop
**File:** `src/units/animation.cpp`
**Function:** `unit_animator::wait_until()` (line 1401)
```cpp
// Lines 1413 and 1420: Called repeatedly during animation
resources::controller->play_slice(false);
```

### 3. Play Slice
**File:** `src/controller_base.cpp`
**Function:** `controller_base::play_slice()` (line 408)
```cpp
// Line 416: Pump events
events::pump();

// Line 417: Raise process events
events::raise_process_event();

// Line 418: Draw - THIS IS THE KEY CALL
events::draw();
```

### 4. Events Draw
**File:** `src/events.cpp`
**Function:** `events::draw()` (line 751)
```cpp
// Line 753: Call draw manager
draw_manager::sparkle();
```

### 5. Draw Manager Sparkle
**File:** `src/draw_manager.cpp`
**Function:** `draw_manager::sparkle()` (line 130)
```cpp
// Line 144: Update all top-level drawables
draw_manager::update();
```

### 6. Draw Manager Update
**File:** `src/draw_manager.cpp`
**Function:** `draw_manager::update()` (line 201)
```cpp
// Line 205: Call update on each top-level drawable
for (size_t i = 0; i < top_level_drawables_.size(); ++i) {
    top_level_drawable* tld = top_level_drawables_[i];
    if (tld) { tld->update(); }
}
```
Note: `game_display` is a top-level drawable.

### 7. Game Display Update
**File:** `src/game_display.cpp`
**Function:** `game_display::update()` (line 172)
```cpp
// Line 174: Call parent update
display::update();

// Line 179: Process reach map changes - THIS PROCESSES THE CHANGES
process_reachmap_changes();
```

### 8. Process Reachmap Changes
**File:** `src/display.cpp`
**Function:** `display::process_reachmap_changes()` (line 3307)
```cpp
// Line 3309: Early return if no changes
if (!reach_map_changed_) return;

// ... process changes ...

// Line 3343: Reset the flag
reach_map_changed_ = false;
```

### 9. Attack Initiation
**File:** `src/mouse_events.cpp`
**Function:** (around line 1003)
After the move completes, the attack is initiated.

**Function:** `mouse_handler::attack_enemy()` (around line 1430)
```cpp
// Line 1430: Try to clear reach map before attack
gui().unhighlight_reach();
```
But at this point, `reach_map_changed_` is already `false` because it was processed during the animation loop, so `process_reachmap_changes()` returns immediately without doing anything.

## Sequence Diagram

```
Move&Attack Action
    |
    v
move_unit_along_current_route() [mouse_events.cpp:1199]
    |
    |--> gui().unhighlight_reach() [line 1206]
    |    (sets reach_map_changed_ = true, clears reach_map_)
    |
    v
move_unit_along_route() -> unit_animator::wait_until() [animation.cpp:1401]
    |
    |--> (Animation Loop - called multiple times)
    |    |
    |    v
    |    controller_base::play_slice() [controller_base.cpp:408]
    |        |
    |        v
    |        events::draw() [events.cpp:751]
    |            |
    |            v
    |            draw_manager::sparkle() [draw_manager.cpp:130]
    |                |
    |                v
    |                draw_manager::update() [draw_manager.cpp:201]
    |                    |
    |                    v
    |                    game_display::update() [game_display.cpp:172]
    |                        |
    |                        v
    |                        process_reachmap_changes() [display.cpp:3307]
    |                        (processes changes, sets reach_map_changed_ = false)
    |
    v
(Move completes, returns to mouse_events.cpp)
    |
    v
attack_enemy() [mouse_events.cpp:1430]
    |
    |--> gui().unhighlight_reach() [line 1430]
         (reach_map_ already empty, reach_map_changed_ already false)
         |
         v
         process_reachmap_changes() [display.cpp:3307]
         (returns immediately at line 3309 due to early return)
```

## Conclusion

The code responsible for processing reach map changes during move&attack is:

**Primary:** `controller_base::play_slice()` in `src/controller_base.cpp:408`

This function is called repeatedly during unit movement animations by `unit_animator::wait_until()`. It calls `events::draw()` which triggers the entire display update chain, ultimately calling `process_reachmap_changes()` during the animation. This processes the reach map changes and sets `reach_map_changed_` to false, causing the subsequent call during attack initiation to return immediately.

The key insight is that **the display update mechanism runs during animations**, not just at discrete game state transitions. This ensures smooth visual updates but also means that reach map changes are processed as soon as the display is updated during the move animation, before the attack code has a chance to run.

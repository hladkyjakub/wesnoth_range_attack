# Code Snippets Reference

Quick reference for the key code sections discussed in the investigation.

## unhighlight_reach() Definition

**File:** `src/game_display.cpp` (lines 554-564)

```cpp
bool game_display::unhighlight_reach()
{
    units_that_can_reach_goal_.clear();
    if(!reach_map_.empty()) {
        reach_map_.clear();
        reach_map_changed_ = true;
        return true;
    } else {
        return false;
    }
}
```

**Key Point:** Only sets `reach_map_changed_ = true` when reach_map_ is not empty.

## process_reachmap_changes() Definition

**File:** `src/display.cpp` (lines 3307-3344)

```cpp
void display::process_reachmap_changes()
{
    if (!reach_map_changed_) return;  // Early return when flag is false
    
    if (reach_map_.empty() != reach_map_old_.empty()) {
        // Invalidate everything except the non-darkened tiles
        reach_map &full = reach_map_.empty() ? reach_map_old_ : reach_map_;

        for (const auto& hex : get_visible_hexes()) {
            reach_map::iterator reach = full.find(hex);
            if (reach == full.end()) {
                // Location needs to be darkened or brightened
                invalidate(hex);
            } else if (reach->second != 1) {
                // Number needs to be displayed or cleared
                invalidate(hex);
            }
        }
    } else if (!reach_map_.empty()) {
        // Invalidate only changes
        reach_map::iterator reach, reach_old;
        for (reach = reach_map_.begin(); reach != reach_map_.end(); ++reach) {
            reach_old = reach_map_old_.find(reach->first);
            if (reach_old == reach_map_old_.end()) {
                invalidate(reach->first);
            } else {
                if (reach_old->second != reach->second) {
                    invalidate(reach->first);
                }
                reach_map_old_.erase(reach_old);
            }
        }
        for (reach_old = reach_map_old_.begin(); reach_old != reach_map_old_.end(); ++reach_old) {
            invalidate(reach_old->first);
        }
    }
    reach_map_old_ = reach_map_;
    reach_map_changed_ = false;  // Reset flag after processing
}
```

**Key Point:** Only processes when `reach_map_changed_` is true, then sets it to false.

## controller_base::play_slice() - The Key Function

**File:** `src/controller_base.cpp` (lines 408-428)

```cpp
void controller_base::play_slice(bool is_delay_enabled)
{
    CKey key;

    if(plugins_context* l = get_plugins_context()) {
        l->play_slice();
    }

    events::pump();
    events::raise_process_event();
    events::draw();  // <-- Triggers display update chain

    // Update sound sources before scrolling
    if(soundsource::manager* l = get_soundsource_man()) {
        l->update();
    }
    
    // ... more code follows ...
}
```

**Key Point:** Called repeatedly during animations, triggers `events::draw()` which leads to `process_reachmap_changes()`.

## Move&Attack Call Sites

### First unhighlight_reach() Call - During Move

**File:** `src/mouse_events.cpp` (line 1206)

```cpp
bool mouse_handler::move_unit_along_current_route()
{
    // Copy the current route to ensure it remains valid throughout the animation.
    const std::vector<map_location> steps = current_route_.steps;

    // do not show footsteps during movement
    gui().set_route(nullptr);
    gui().unhighlight_reach();  // <-- First call: reach_map not empty

    // do not keep the hex highlighted that we started from
    selected_hex_ = map_location();
    gui().select_hex(map_location());
    
    // ... animation and movement code follows ...
}
```

### Second unhighlight_reach() Call - Before Attack

**File:** `src/mouse_events.cpp` (line 1430)

```cpp
void mouse_handler::attack_enemy(...)
{
    // ... setup code ...

    // remove highlighted hexes etc..
    gui().select_hex(map_location());
    gui().highlight_hex(map_location());
    gui().clear_attack_indicator();
    gui().unhighlight_reach();  // <-- Second call: reach_map already empty

    // ... attack code follows ...
}
```

## Animation Loop Call Site

**File:** `src/units/animation.cpp` (lines 1413, 1420)

```cpp
void unit_animator::wait_until(int animation_time) const
{
    if(animated_units_.empty()) {
        return;
    }
    
    // ... setup code ...
    
    resources::controller->play_slice(false);  // <-- Initial call

    int end_tick = animated_units_[0].my_unit->anim_comp().get_animation()->time_to_tick(animation_time);
    while(SDL_GetTicks() < unsigned(end_tick - std::min(int(20 / speed), 20))) {
        if(!game_config::no_delay) {
            SDL_Delay(std::clamp(int((animation_time - get_animation_time()) * speed), 0, 10));
        }
        resources::controller->play_slice(false);  // <-- Repeated calls in loop
        end_tick = animated_units_[0].my_unit->anim_comp().get_animation()->time_to_tick(animation_time);
    }
    
    // ... cleanup code ...
}
```

**Key Point:** `play_slice()` is called multiple times during the animation loop, each time potentially triggering display updates.

## Display Update Chain

The chain triggered by `play_slice()`:

```cpp
controller_base::play_slice()          // src/controller_base.cpp:408
    ↓
events::draw()                         // src/events.cpp:751
    ↓
draw_manager::sparkle()                // src/draw_manager.cpp:130
    ↓
draw_manager::update()                 // src/draw_manager.cpp:201
    ↓
game_display::update()                 // src/game_display.cpp:172
    ↓
display::process_reachmap_changes()    // src/display.cpp:3307
```

Each step in this chain calls the next, ultimately reaching `process_reachmap_changes()`.

# Understanding unhighlight_reach() and reach_map_changed_

## Question
Where is `gui().unhighlight_reach()` defined? It sets `reach_map_changed_` to true, and only `process_reachmap_changes()` sets it to false. So wouldn't that imply that `process_reachmap_changes()` is called twice in a row with nothing in between that sets `reach_map_changed_` to true?

## Answer

### Location of unhighlight_reach()

**Defined in:** `src/game_display.cpp` (line 554)  
**Declared in:** `src/game_display.hpp` (line 104)

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

### Key Insight: Conditional Flag Setting

The crucial detail is that `unhighlight_reach()` only sets `reach_map_changed_ = true` **if the reach_map_ is NOT empty**. If the reach_map is already empty, it does nothing and returns false.

## The Move&Attack Scenario Explained

Let's trace what happens during a move-and-attack action:

### Step 1: Move Initiation (First unhighlight_reach call)
**Location:** `src/mouse_events.cpp:1206` in `move_unit_along_current_route()`

```cpp
gui().unhighlight_reach();  // First call
```

**What happens:**
- `reach_map_` is NOT empty (contains movement range)
- `reach_map_.clear()` is executed
- `reach_map_changed_ = true` is set
- Returns `true`

### Step 2: Animation Loop
**Location:** Animation processing triggers display updates

During the unit movement animation, `controller_base::play_slice()` is called repeatedly, which triggers:
```
play_slice() → events::draw() → draw_manager::sparkle() 
→ draw_manager::update() → game_display::update() 
→ process_reachmap_changes()
```

**What happens in process_reachmap_changes():**
- Sees `reach_map_changed_ == true`
- Processes the changes (invalidates hexes)
- Sets `reach_map_old_ = reach_map_` (both are now empty)
- Sets `reach_map_changed_ = false`

### Step 3: Attack Initiation (Second unhighlight_reach call)
**Location:** `src/mouse_events.cpp:1430` in `attack_enemy()`

```cpp
gui().unhighlight_reach();  // Second call
```

**What happens:**
- `reach_map_` IS ALREADY EMPTY (cleared in Step 1)
- The `if(!reach_map_.empty())` condition is **false**
- Nothing is executed - no clear, no flag change
- Returns `false`

### Step 4: No Second Processing
When `process_reachmap_changes()` is potentially called again later:
```cpp
if (!reach_map_changed_) return;  // Exits immediately
```

**Why:** Because `reach_map_changed_` was never set to `true` in Step 3 (the reach_map was already empty).

## Conclusion

`process_reachmap_changes()` is **NOT** called twice in a row with nothing in between. Here's what actually happens:

1. First `unhighlight_reach()`: Sets flag to true (reach_map was not empty)
2. First `process_reachmap_changes()`: Processes changes, sets flag to false
3. Second `unhighlight_reach()`: Does NOT set flag to true (reach_map already empty)
4. Second `process_reachmap_changes()`: Returns immediately (flag is still false)

The second call to `unhighlight_reach()` is essentially a no-op because the reach_map was already cleared by the first call. This is the intended behavior - calling `unhighlight_reach()` on an already-unhighlighted reach map is safe and doesn't trigger unnecessary processing.

## Why This Design?

This design prevents redundant work:
- If the reach_map is already empty, there's nothing to unhighlight
- No need to trigger display invalidations for a no-op
- Multiple calls to `unhighlight_reach()` are safe and idempotent after the first

## Related Files

- **Definition:** `src/game_display.cpp:554-564`
- **Declaration:** `src/game_display.hpp:104`
- **Processing:** `src/display.cpp:3307-3344` (process_reachmap_changes)
- **Usage Examples:** `src/mouse_events.cpp:1206, 1430, 1573` (and many others)

## Visual Flow

```
Move&Attack Sequence:
┌─────────────────────────────────────┐
│ Move Initiation (line 1206)         │
│ gui().unhighlight_reach()           │
│   reach_map_.empty() = false        │
│   → reach_map_.clear()              │
│   → reach_map_changed_ = true ✓     │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ Animation Loop                      │
│ process_reachmap_changes()          │
│   reach_map_changed_ == true        │
│   → Process invalidations           │
│   → reach_map_changed_ = false      │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ Attack Initiation (line 1430)       │
│ gui().unhighlight_reach()           │
│   reach_map_.empty() = true ✗       │
│   → if condition fails              │
│   → NO flag change                  │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│ Later calls to                      │
│ process_reachmap_changes()          │
│   reach_map_changed_ == false       │
│   → Early return (line 3309)        │
└─────────────────────────────────────┘
```

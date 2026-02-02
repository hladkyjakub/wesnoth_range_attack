# Reachmap Processing Investigation - Documentation Index

This directory contains comprehensive documentation of the reachmap processing behavior during move&attack actions in the Wesnoth codebase.

## Quick Navigation

### 🎯 Start Here
- **[INVESTIGATION_SUMMARY.md](INVESTIGATION_SUMMARY.md)** - Overview of both questions and complete answers

### 📚 Detailed Documentation
1. **[QUICK_ANSWER.md](QUICK_ANSWER.md)** - Concise answers with key file references (2.4 KB)
2. **[REACHMAP_ANALYSIS.md](REACHMAP_ANALYSIS.md)** - Complete call chain analysis (7.8 KB)
3. **[UNHIGHLIGHT_REACH_EXPLANATION.md](UNHIGHLIGHT_REACH_EXPLANATION.md)** - Detailed flag logic explanation (5.9 KB)
4. **[CODE_SNIPPETS.md](CODE_SNIPPETS.md)** - Code reference with all key functions (5.7 KB)
5. **[call_chain_diagram.txt](call_chain_diagram.txt)** - ASCII flow diagram (2.3 KB)

## The Questions

### Question 1
> When reachmap is updated, the `process_reachmap_changes()` doesn't immediately return - this is true for all cases, except for the case when unit performs the move&attack - the reachmap gets wiped, but `process_reachmap_changes()` does return right at the start. What code is responsible for this?

**Answer:** `controller_base::play_slice()` in `src/controller_base.cpp:408`

### Question 2
> Where is `gui().unhighlight_reach()` defined? It does set the `reach_map_changed_` to true, only place that sets it to false is `process_reachmap_changes()`. So that would imply that `process_reachmap_changes()` is called two times in a row, and in between is nothing that would set `reach_map_changed_` to true executed.

**Answer:** `game_display::unhighlight_reach()` in `src/game_display.cpp:554`

The key insight: It only sets the flag when reach_map is NOT empty, preventing redundant processing.

## Key Findings

### 1. Display Updates During Animations
Display updates happen **during** unit movement animations via `controller_base::play_slice()`, not just at discrete game state transitions.

Call chain:
```
play_slice() → events::draw() → draw_manager::sparkle() 
→ draw_manager::update() → game_display::update() 
→ process_reachmap_changes()
```

### 2. Conditional Flag Setting
`unhighlight_reach()` only sets `reach_map_changed_ = true` if the reach_map is NOT empty:

```cpp
bool game_display::unhighlight_reach()
{
    if(!reach_map_.empty()) {
        reach_map_.clear();
        reach_map_changed_ = true;  // Only here!
        return true;
    } else {
        return false;  // No flag change
    }
}
```

### 3. Why No Double Processing
During move&attack:
1. **First call** (move): reach_map not empty → clears it, sets flag = true
2. **Animation**: `process_reachmap_changes()` processes, sets flag = false
3. **Second call** (attack): reach_map ALREADY empty → does NOT set flag = true
4. **Result**: No second processing occurs (flag stays false)

This is NOT a case of double processing with nothing in between. The second call is a no-op by design.

## Code Locations Quick Reference

| Component | File | Lines |
|-----------|------|-------|
| unhighlight_reach() definition | src/game_display.cpp | 554-564 |
| unhighlight_reach() declaration | src/game_display.hpp | 104 |
| process_reachmap_changes() | src/display.cpp | 3307-3344 |
| play_slice() | src/controller_base.cpp | 408-428 |
| Move initiation (1st call) | src/mouse_events.cpp | 1206 |
| Attack initiation (2nd call) | src/mouse_events.cpp | 1430 |
| Animation loop | src/units/animation.cpp | 1413, 1420 |

## Documentation Structure

```
Documentation/
├── README_REACHMAP_DOCS.md          ← You are here
├── INVESTIGATION_SUMMARY.md         ← Start here for overview
├── QUICK_ANSWER.md                  ← Quick reference
├── REACHMAP_ANALYSIS.md             ← Question 1 deep dive
├── UNHIGHLIGHT_REACH_EXPLANATION.md ← Question 2 deep dive
├── CODE_SNIPPETS.md                 ← Code reference
└── call_chain_diagram.txt           ← Visual flow
```

## How to Read This Documentation

1. **For a quick answer**: Read [QUICK_ANSWER.md](QUICK_ANSWER.md)
2. **For understanding Question 1**: Read [REACHMAP_ANALYSIS.md](REACHMAP_ANALYSIS.md)
3. **For understanding Question 2**: Read [UNHIGHLIGHT_REACH_EXPLANATION.md](UNHIGHLIGHT_REACH_EXPLANATION.md)
4. **For code lookup**: Check [CODE_SNIPPETS.md](CODE_SNIPPETS.md)
5. **For visual flow**: View [call_chain_diagram.txt](call_chain_diagram.txt)
6. **For complete context**: Read [INVESTIGATION_SUMMARY.md](INVESTIGATION_SUMMARY.md)

## Summary

The investigation reveals that:

1. **Reachmap processing happens during animations** through the event loop
2. **The design is intentional** - `unhighlight_reach()` is idempotent and safe to call multiple times
3. **No redundant work occurs** - the flag system prevents unnecessary processing
4. **The code is well-designed** - display updates are smooth and efficient

This behavior is not a bug but an intentional design choice that makes the display update system robust and efficient.

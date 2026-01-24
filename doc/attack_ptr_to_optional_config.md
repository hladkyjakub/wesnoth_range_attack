# Converting const_attack_ptr to optional_const_config

## Overview

This document explains how to convert between `const_attack_ptr` and `optional_const_config` when working with weapon data in events.

## Types Involved

- **`const_attack_ptr`**: `std::shared_ptr<const attack_type>` - A smart pointer to an attack object
- **`config`**: The WML configuration object that stores structured data
- **`optional_const_config`**: `optional_config_impl<const config>` - An optional reference to a const config

## The Conversion Pattern

### From const_attack_ptr to config

Use the helper function `attack_config()`:

```cpp
#include "units/attack_type.hpp"  // Provides the implementation

const_attack_ptr weapon = /* ... */;
config weapon_cfg = attack_config(weapon);  // Handles null case automatically
```

This function internally uses `attack_type::to_config()` and handles null pointers by returning an empty `config`.

### Storing in Event Data

To pass weapon data to event handlers:

```cpp
config dat;
dat.add_child("first", attack_config(attacker_weapon));
dat.add_child("second", attack_config(defender_weapon));

// Fire event with weapon data
resources::game_events->pump().fire("attack_end", loc1, loc2, dat);
```

### Retrieving as optional_const_config

In event handlers (like in `game_events/pump.cpp`):

```cpp
// From ev.data, retrieve weapon configs as optional references
scoped_weapon_info first_weapon("weapon", ev.data.optional_child("first"));
scoped_weapon_info second_weapon("second_weapon", ev.data.optional_child("second"));
```

Or directly in Lua kernel:

```cpp
if (auto weapon = ev.data.optional_child("first")) {
    // weapon is an optional_const_config
    // Use *weapon to dereference to const config&
    cfg.add_child("weapon", *weapon);
}
```

## Complete Example

```cpp
// In combat code (e.g., actions/attack.cpp):
void perform_attack(const_attack_ptr attacker_weapon, 
                   const_attack_ptr defender_weapon) {
    // Convert attack pointers to configs for event data
    config event_data;
    event_data.add_child("first", attack_config(attacker_weapon));
    event_data.add_child("second", attack_config(defender_weapon));
    
    // Fire event
    resources::game_events->pump().fire("weapon_clash", loc, loc2, event_data);
}

// In event handling code (e.g., game_events/pump.cpp):
void handle_event(const queued_event& ev) {
    // Retrieve weapons as optional_const_config
    optional_const_config first = ev.data.optional_child("first");
    optional_const_config second = ev.data.optional_child("second");
    
    // Check if weapons exist
    if (first) {
        // Access weapon data
        std::string weapon_name = (*first)["name"].str();
    }
}
```

## Implementation Details

The `attack_config()` function is:
- **Declared** in `src/units/ptr.hpp` (alongside `const_attack_ptr` typedef)
- **Implemented** in `src/units/attack_type.hpp` (where both `attack_type` and `config` are fully defined)

This split declaration/implementation avoids circular dependencies while keeping the API convenient.

## See Also

- `src/units/ptr.hpp` - Attack pointer type definitions
- `src/units/attack_type.hpp` - Attack type class and attack_config() implementation
- `src/config.hpp` - Config and optional_const_config definitions
- `src/actions/attack.cpp` - Example usage in combat
- `src/game_events/pump.cpp` - Example usage in event handling

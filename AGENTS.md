# AGENTS.md -- Swim

A native Motor Town plugin: one DLL loaded by
[Plugin Loader](https://github.com/qxZap/mt-plugin-loader). Source is
`plugins/swim.cpp`; `plugins/sea_waves.hpp` is the wave and height model it
shares with the Boats plugin. `include/` is a copy of the loader's headers;
refresh it from mt-plugin-loader when the api version changes.

## Build and test

`build.bat` (MSVC 2022 x64) writes `out\`. Install to `MotorTown\Binaries\Win64\plugins\`
with the game closed. There is no test suite: the log `plugins\mt_plugin_Swim.log` in that
folder is the check, so log what you need to see in game. The plugin's own log
is `plugins\logs\Swim\<timestamp>.log`.

## Rules

- The loader owns the ProcessEvent hook and engine discovery. Use `include/mt_plugin_api.h`,
  and never hook a vtable from a plugin.
- `process_event` only on the game thread (`run_on_game_thread`).
- Find fields by reflection (`api->field_offset`) and code by signature (`find_sig`), never
  by address. A missing field or signature logs a line and leaves the game vanilla.
- Never look a field up inside the frame: the loader rebuilds a field map on each call.
- Hardcoded offsets are the exception and need a `shortcut:` comment naming the limit.
- A plugin rewriting game data affects the machine running the game: single player or the
  host. On someone else's server that server's rules apply.
- Settings live in `Binaries\Win64\plugins\swim.ini`, through the loader's ini functions.
  `[General] Disabled=1` makes `PluginInit` start nothing.

## This plugin

The character's movement goes to flying and the plugin owns the vertical: it holds the body
at the float line, rides the wave, and gives the movement back when the swim ends. The
character can still be standing on a vehicle the game destroyed, so the movement base is
cleared before every mode change and the game gets a frame of falling first -- that was
five crashes in a row (the game reads a destroyed object as the base).

The wave model is the game's own: the ocean's Gerstner wave list, read off the water body
and summed the way the water material sums it, on the clock the water itself runs on.

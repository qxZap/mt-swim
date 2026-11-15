> **Merged into [mt-sea](https://github.com/qxZap/mt-sea) (2026-11-15).** The Swim and Boats plugins are one
> plugin now -- one sea state, one scan, one DLL. This repository is kept as the history of the swim half.

# Swim

Swimming on foot.

The character floats in the water instead of walking on the bottom: it rides the
sea's own waves (the same wave model the Boats plugin floats hulls on), leans
with them, swims with the arms, dives, runs out of air and comes up.

The water it floats on is the water you can see: the ocean mesh's own level,
not the water body's collision surface, which UE keeps above the mesh so the
wave crests cannot poke through it. The float line is a height on the body
(a chest deep), measured off the character's own mesh, so a short character and
a tall one both sit right.

Settings: `Binaries\Win64\plugins\swim.ini`.

| key | what |
|---|---|
| `FloatDepth` / `FreshFloatDepth` | how far under the surface the body's centre sits (salt / fresh water) |
| `WaterLine` | where the water sits on the body, as a fraction of the body's height from the feet (0.78 is the chest, Breath of the Wild's) |
| `Lean` | how far the body may lean with the wave, in degrees (0 = the games' way: heave without tilting) |
| `Speed` / `SprintScale` | swimming speed, and the multiplier while Shift is held |
| `Stamina` | seconds of sprint before the rest |
| `Oxygen` | seconds under water before the blackout |
| `DiveSpeed` / `RiseSpeed` / `Drift` | down, up, and the buoyancy that lifts you when you let go |
| `JumpOut` | how hard Space throws you out of the water |
| `DiveKey` | the dive key (17 = Ctrl) |
| `CrawlMontage` / `TreadMontage` / `DiveMontage` | the animation assets; the defaults are the pak `qxZap_Swim_P` |

## Install

Needs [Plugin Loader](https://github.com/qxZap/mt-plugin-loader) (`version.dll`) in
`MotorTown\Binaries\Win64\`. Put `mt_plugin_Swim.dll` in `MotorTown\Binaries\Win64\plugins\`.
The animations are the pak `qxZap_Swim_P`; without it the character glides upright.

## Build

`build.bat` (MSVC 2022, x64) writes `out\mt_plugin_Swim.dll`.

## Notes

The swimming pose is local: other players see you glide. The jump out of the
water is what gets you aboard a moored boat for now.

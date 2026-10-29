# Uranium

A new Geode mod project for Geometry Dash 2.2081, targeting Windows and Android64.

## Included in this starter build
- Uranium mod metadata and build target.
- Experimental .gpt macro recording/playback UI, speed control, and level-completion auto-save logic.
- A prototype hitbox/pathfinder toggle retained from the source project.

## Important status note
This is a renamed starter project, not a finished automatic solver. The current pathfinder toggle does **not** yet map every collision hitbox, calculate a complete route, retry intelligently until 100%, or provide the requested pre-level Solve / Replay / Play Normally menu. Those need further implementation and testing against the target Geode SDK. Macro playback and speed controls are experimental; they have not been compile-tested here.

## Physics reference
The project is intended to use camila314/gdp as the physics reference, with permission stated by the project requester. Reference: https://github.com/camila314/gdp. No gdp source code is bundled in this ZIP.

## Build
Set GEODE_SDK to your installed Geode SDK, then configure/build with CMake as for a standard Geode mod.

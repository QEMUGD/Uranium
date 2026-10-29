# Uranium

A clean-room Geometry Dash Geode mod project. This project is written from scratch and does not use GPTShowcase code.

## Planned features
- A pre-level chooser with **Solve**, **Replay**, and **Play Normally**.
- Record, save, select, and replay macros.
- Save the active macro when a level completes.
- Level geometry/hitbox collection and route-planning infrastructure.
- Configurable game-speed / TPS experimentation.
- Physics research based on the separately maintained `camila314/gdp` project: https://github.com/camila314/gdp (used as a reference; its source is not copied into this repository).

## Status
This is a clean initial implementation, not yet a finished auto-solver. Accurate physics simulation, collision-faithful path search, automatic retries until a verified 100% completion, and real TPS bypass still require implementation and testing against the exact Geometry Dash and Geode SDK versions. Do not treat a planned feature as working until a release build and in-game test verify it.

## Build
Build with the Geode SDK for the supported Geometry Dash version. Set `GEODE_SDK` as required by your SDK setup and use the Geode CMake build workflow.

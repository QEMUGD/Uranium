# Uranium architecture

This clean-room project separates the features so the pathfinder cannot silently claim success without evidence.

1. **Macro format** — timestamped button-down/button-up events, with explicit versioning.
2. **Level map** — record each visible object's bounding rectangle and object ID. A map is an observation, not a guarantee that every gameplay collision rule is known.
3. **Route solver** — search candidate inputs against a physics model. The solver must validate collisions and game mode changes, not merely draw a line around hitboxes.
4. **Replay verifier** — replay a candidate, observe the actual result, and mark it successful only when the game reports level completion.
5. **Retry loop** — retain the best candidate, restart after failure, and stop only at confirmed completion or a configured safety limit.
6. **Pre-level UI** — Solve; Replay (disabled when no saved replay exists); Play Normally.
7. **Speed / TPS experiments** — keep separate from macro timestamps and label them experimental until verified.

The first commit is scaffolding. The real-time physics model and fully automated solver are not implemented yet.

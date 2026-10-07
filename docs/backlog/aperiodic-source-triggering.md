# Trigger aperiodic sources

**Status:** 🟢 Ready (once there's an actual need for one)

## What needs to be done

`src/main.cpp` re-releases periodic sources (`period_us > 0`) via one
dedicated thread per source. Aperiodic sources (`period_us == 0`) have no
trigger at all today — no network listener, no timer with a different
signal, nothing. Add one, once there's a concrete aperiodic subtask to
drive (a network event, a different timer signal, etc.).

## Why

Named limitation in `main.cpp`'s own header comment, not a silent gap —
but it means no aperiodic subtask can actually run end-to-end today.

## Relevant files

`src/main.cpp`.

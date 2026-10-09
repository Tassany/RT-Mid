# Fuzz the deployment plan parser

**Status:** 🟢 Ready

## What needs to be done

Write a simple mutation-based fuzzer (malformed/truncated JSON, wrong
field types — no need for libFuzzer/AFL) against `JsonParser::parse()`.

## Why

`JsonParser::parse()` handles external input (the deployment plan) and has
only ever been exercised against well-formed plans. This catches
crashes/UB on hostile input in the Configuration layer. Full rationale:
[../testing-strategy.md](../testing-strategy.md) (item 5).

## Relevant files

`src/parser_json.cpp`, `src/parser_json.hpp`.

# Deterministic profile and gas metering

This fork adds a deterministic execution mode plus a built-in gas counter that runs inside the interpreter and a handful of C builtins.

## New public C API

- `JSContext *JS_NewDeterministicContext(JSRuntime *rt);` (returns a context with the deterministic profile installed)
- `int JS_AddIntrinsicDeterministic(JSContext *ctx);` (useful if you start from `JS_NewContextRaw`)
- `void JS_SetDeterministicMode(JSContext *ctx, int enabled);`
- `void JS_SetDeterministicProfileId(JSContext *ctx, uint32_t profile_id);`
- `void JS_SetDeterministicRandomSeed(JSContext *ctx, uint64_t seed);`
- `void JS_SetGasLimit(JSContext *ctx, uint64_t limit);`
- `void JS_ResetGas(JSContext *ctx);`
- `uint64_t JS_GetGasUsed(JSContext *ctx);`
- `uint64_t JS_GetGasRemaining(JSContext *ctx);`
- `void JS_SetGasSchedule(JSContext *ctx, uint32_t schedule_id, const uint16_t *cost_table, size_t table_len);`

### Deterministic profile behavior

- Only base intrinsics are installed (Objects, Numbers, etc.). Promises, Proxies, RegExp, Date, TypedArrays, etc. are *not* added.
- `Function` constructor and direct `eval` are disabled; direct eval is rejected even if injected.
- RegExp literals, async/await, and dynamic import are rejected at parse time.
- `Math.random` is seeded deterministically (default seed `1` unless you call `JS_SetDeterministicRandomSeed`).
- GC auto-triggering is disabled when deterministic mode is on; hosts should call `JS_RunGC` at deterministic checkpoints.

### Gas accounting behavior

- Gas state lives on the context. Execution is poisoned on out-of-gas, so even if JS catches an exception, further execution fails.
- Opcodes are charged *before* execution in the interpreter loop using the current schedule (defaults to `1` per opcode).
- Allocations go through the gas meter (requested size is charged).
- Hot C builtins that loop over user data (Array map/filter/reduce, TypedArray variants, iterator reduce) charge a base + per-element cost.

## Minimal C example

`examples/deterministic_demo.c` demonstrates:

1. Creating a deterministic context (intrinsics included).
2. Setting a gas limit and schedule.
3. Running a script and observing gas consumption.
4. Observing out-of-gas and direct-eval denial.

Build and run (one-shot example build):

```sh
cc -std=c11 -Wall -Wextra -I. quickjs.c quickjs-libc.c examples/deterministic_demo.c -o deterministic_demo -lm -ldl
./deterministic_demo
```

Or use the Makefile target:

```sh
make examples/deterministic_demo
./examples/deterministic_demo
```

Expected output (abbreviated):

```
result: 6
gas used: <non-zero>, remaining: <limit - used>
direct eval error: direct eval is disabled in deterministic mode
out of gas error observed
```

You can also point the `gas_schedule` at your own cost table via `JS_SetGasSchedule` to version costs per deployment.

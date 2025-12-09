#include "quickjs.h"
#include "quickjs-libc.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

static void dump_exception(JSContext *ctx)
{
    JSValue exc = JS_GetException(ctx);
    const char *str = JS_ToCString(ctx, exc);
    if (str) {
        fprintf(stdout, "exception: %s\n", str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, exc);
}

static void run_case(JSContext *ctx, const char *label, const char *code,
                     uint64_t gas_limit)
{
    JS_SetGasLimit(ctx, gas_limit);
    JS_ResetGas(ctx);
    JSValue val = JS_Eval(ctx, code, strlen(code), label, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
        fprintf(stdout, "%s: ", label);
        dump_exception(ctx);
    } else {
        const char *res = JS_ToCString(ctx, val);
        fprintf(stdout, "%s: result=%s\n", label, res ? res : "<non-string>");
        JS_FreeCString(ctx, res);
        JS_FreeValue(ctx, val);
    }
    fprintf(stdout, "%s: gas used=%" PRIu64 ", remaining=%" PRIu64 "\n",
            label, JS_GetGasUsed(ctx), JS_GetGasRemaining(ctx));
}

int main(void)
{
    const char *src = "const x = 1 + 2 + 3; x;";
    const char *map_concat = "[1,2,3].map((n,i)=>'v'+(n+i))";
    const char *filter_concat = "['ab','c','def','g'].filter((s,i)=> (s+i).length%2===0).join('|')";
    const char *reduce_concat = "['a','b','c'].reduce((acc,s,i)=> acc + s + i, '')";
    const char *nested_map = "[[1,2],[3]].map(xs=> xs.map(x=>'x'+x).join('+')).join('|')";
    const char *oog_large_limit = "let accLargeLimit = 0; for (let i = 0; i < 1000; i++) accLargeLimit += i; accLargeLimit;";
    const char *oog_small_limit = "let accSmallLimit = 0; for (let i = 0; i < 1000; i++) accSmallLimit += i; accSmallLimit;";
    const char *missing_promise = "typeof Promise";
    const char *missing_ta = "typeof Uint8Array";
    const char *function_ctor = "Function('return 1')";
    const char *regexp_literal = "/abc/.test('abc')";
    const char *regexp_ctor = "new RegExp('abc')";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewDeterministicContext(rt);
    if (!ctx) {
        fprintf(stderr, "failed to create deterministic context\n");
        return 1;
    }
    fprintf(stdout, "created deterministic context\n");

    /* Optional schedule override: leave NULL to use default cost of 1/opcode. */
    JS_SetGasSchedule(ctx, 1, NULL, 0);

    run_case(ctx, "arith", src, 10000);
    run_case(ctx, "map+concat", map_concat, 10000);
    run_case(ctx, "filter+concat", filter_concat, 10000);
    run_case(ctx, "reduce+concat", reduce_concat, 10000);
    run_case(ctx, "nested map", nested_map, 10000);
    run_case(ctx, "direct eval", "eval('1 + 1')", 10000);
    run_case(ctx, "out of gas - larger limit", oog_large_limit, 20000);
    run_case(ctx, "out of gas - small limit", oog_small_limit, 10000);
    run_case(ctx, "missing Promise", missing_promise, 5000);
    run_case(ctx, "missing typed array", missing_ta, 5000);
    run_case(ctx, "Function constructor", function_ctor, 5000);
    run_case(ctx, "regexp literal", regexp_literal, 5000);
    run_case(ctx, "RegExp constructor", regexp_ctor, 5000);

    /* Deterministic Math.random(): same seed -> same output */
    const char *random_pair = "[Math.random(), Math.random()]";
    JS_SetGasLimit(ctx, 5000);
    JS_ResetGas(ctx);
    JS_SetDeterministicRandomSeed(ctx, 1234);
    JSValue r1 = JS_Eval(ctx, random_pair, strlen(random_pair), "<rand1>", JS_EVAL_TYPE_GLOBAL);
    const char *r1s = JS_IsException(r1) ? NULL : JS_ToCString(ctx, r1);
    JS_FreeValue(ctx, r1);

    JS_SetGasLimit(ctx, 5000);
    JS_ResetGas(ctx);
    JS_SetDeterministicRandomSeed(ctx, 1234);
    JSValue r2 = JS_Eval(ctx, random_pair, strlen(random_pair), "<rand2>", JS_EVAL_TYPE_GLOBAL);
    const char *r2s = JS_IsException(r2) ? NULL : JS_ToCString(ctx, r2);
    JS_FreeValue(ctx, r2);

    if (r1s && r2s && strcmp(r1s, r2s) == 0) {
        fprintf(stdout, "random deterministic: %s (matched)\n", r1s);
    } else {
        fprintf(stdout, "random deterministic: mismatch r1=%s r2=%s\n",
                r1s ? r1s : "<err>", r2s ? r2s : "<err>");
    }
    if (r1s)
        JS_FreeCString(ctx, r1s);
    if (r2s)
        JS_FreeCString(ctx, r2s);


    fprintf(stdout, "cleaning up\n");
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}

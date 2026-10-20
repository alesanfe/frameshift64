/**
 * coroutine_emscripten.cpp — Emscripten-fiber coroutine backend.
 *
 * WebAssembly has no ucontext/fibers of its own; Emscripten provides
 * emscripten_fiber_* (emscripten/fiber.h), a C-coroutine primitive with an
 * explicitly-sized C stack plus an Asyncify stack — a 1:1 match for this
 * port's contract (resume->yield pairs, entry-return finishes).
 *
 * Requires -sASYNCIFY (fibers are built on it) and enough
 * ASYNCIFY_STACK_SIZE for the deepest frame live at a yield site. The fiber
 * C stack holds the real call frames; Asyncify only needs room for the
 * unwind data.
 *
 * Semantics mirror coroutine_posix.cpp: resume() records the resuming
 * fiber as `caller`, yield() swaps back to it — so coroutine->coroutine
 * resume works the same as main->coroutine.
 *
 * Guarded by __EMSCRIPTEN__; compiled empty elsewhere.
 */

#ifdef __EMSCRIPTEN__

#include "coroutine.h"
#include "coroutine_tracking.h"
#include "port_log.h"
#include "port_watchdog.h"

#include <emscripten/fiber.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stack-canary: fibers grow DOWN from the top of their malloc'd
 * C stack — an overflow writes past the LOW end into the preceding heap
 * block (silent corruption of FILEs/locks, seen as hangs in stdio).
 * SAFE_HEAP inflates every wasm frame, so stacks sized for native may
 * not be enough here. A sentinel at the low end is checked on swap
 * return and at coroutine finish. */
#define STACK_CANARY_WORDS 16
#define STACK_CANARY_MAGIC 0xC0FFEE42u
#define STACK_CANARY_BYTES (STACK_CANARY_WORDS * 4)

struct PortCoroutine {
	emscripten_fiber_t fiber;
	emscripten_fiber_t *caller;     /* fiber that resumed us — yield target */
	PortCoroutine *caller_co;       /* resumer's PortCoroutine, NULL if main */
	void *c_stack;                  /* fiber's real call stack (malloc'd) */
	void *asyncify_stack;           /* fiber's asyncify stack (malloc'd) */
	size_t asyncify_size;
	void (*entry)(void *);          /* entry point */
	void *arg;
	int finished;                   /* entry returned */
	PortCoroutine *next_live;       /* registry link (rollback reap) */
	uint32_t serial;                /* creation order */
};

/* Running fiber on THIS thread, NULL outside a coroutine. thread_local is
 * load-bearing: real pthread workers must always report "not in a
 * coroutine" so port_log and the N64 stubs take their normal (locked)
 * path even while the main thread is suspended inside a fiber. */
static thread_local PortCoroutine *sCurrent;
static emscripten_fiber_t sMainFiber;
static int sMainReady;
static uint32_t sNextSerial;
static PortCoroutine *sLive;        /* intrusive list, newest first */

#define ASYNCIFY_STACK_BYTES (512 * 1024)
#define MAIN_ASYNCIFY_STACK_BYTES (256 * 1024)
static void *sMainAsyncifyStack;

static void stack_canary_init(PortCoroutine *co)
{
	uint32_t *w = (uint32_t *)co->c_stack;
	for (int i = 0; i < STACK_CANARY_WORDS; i++) {
		w[i] = STACK_CANARY_MAGIC;
	}
}

static void stack_canary_check(PortCoroutine *co, const char *where)
{
	const uint32_t *w = (const uint32_t *)co->c_stack;
	for (int i = 0; i < STACK_CANARY_WORDS; i++) {
		if (w[i] != STACK_CANARY_MAGIC) {
			fprintf(stderr,
			        "CORO-STACK-OVERFLOW co=%p serial=%u where=%s "
			        "canary[%d]=0x%08x (stack low end smashed)\n",
			        (void *)co, co->serial, where, i, w[i]);
			return;
		}
	}
}

static void coroutine_trampoline(void *arg)
{
	PortCoroutine *co = (PortCoroutine *)arg;

	fprintf(stderr, "CT%d tramp co=%p entry=%p\n", co->serial, (void *)co,
	        (void *)co->entry);
	co->entry(co->arg);
	stack_canary_check(co, "exit");
	co->finished = 1;
	sCurrent = co->caller_co;
	/* entry returned: swap back to whoever resumed us and never come back —
	 * matching ucontext's uc_link semantics, a second resume just returns. */
	emscripten_fiber_swap(&co->fiber, co->caller);
}

void port_coroutine_init_main(void)
{
	if (sMainReady) {
		return;
	}
	/* The main fiber's C stack is the real stack; it still needs its own
	 * asyncify stack because coroutine yields swap back into it. */
	if (sMainAsyncifyStack == NULL) {
		sMainAsyncifyStack = malloc(MAIN_ASYNCIFY_STACK_BYTES);
	}
	emscripten_fiber_init_from_current_context(&sMainFiber,
	                                           sMainAsyncifyStack,
	                                           MAIN_ASYNCIFY_STACK_BYTES);
	sMainReady = 1;
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size)
{
	PortCoroutine *co;

	if (stack_size < 32768) {
		stack_size = 32768;
	}
	co = (PortCoroutine *)calloc(1, sizeof(*co));
	if (co == NULL) {
		return NULL;
	}
	co->c_stack = malloc(stack_size + STACK_CANARY_BYTES);
	co->asyncify_stack = malloc(ASYNCIFY_STACK_BYTES);
	co->asyncify_size = ASYNCIFY_STACK_BYTES;
	if (co->c_stack == NULL || co->asyncify_stack == NULL) {
		free(co->c_stack);
		free(co->asyncify_stack);
		free(co);
		return NULL;
	}
	co->entry = entry;
	co->arg = arg;
	stack_canary_init(co);
	/* The fiber stack starts above the canary guard zone — its usable
	 * range stays [c_stack+CANARY, c_stack+CANARY+stack_size). */
	emscripten_fiber_init(&co->fiber, coroutine_trampoline, co,
	                      (char *)co->c_stack + STACK_CANARY_BYTES, stack_size,
	                      co->asyncify_stack, co->asyncify_size);
	coro::track(co, sLive, sNextSerial);
	return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
	if (co == NULL) {
		return;
	}
	coro::untrack(co, sLive);
	free(co->c_stack);
	free(co->asyncify_stack);
	free(co);
}

void port_coroutine_resume(PortCoroutine *co)
{
	emscripten_fiber_t *resumer;

	if (co == NULL || co->finished) {
		return;
	}
	PortCoroutine *prev = sCurrent;

	resumer = (prev != NULL) ? &prev->fiber : &sMainFiber;
	co->caller = resumer;
	co->caller_co = prev;
	sCurrent = co;
	fprintf(stderr, "CT%d resume swap-in\n", co->serial);
	emscripten_fiber_swap(resumer, &co->fiber);
	fprintf(stderr, "CT%d resume swap-back\n", co->serial);
	stack_canary_check(co, "resume-return");
	/* Control returns here when `co` yields or finishes. Restore the
	 * resumer's coroutine identity — NULL when that was the main fiber.
	 * This is what makes nested fiber->fiber resumes keep
	 * port_coroutine_in_coroutine() true inside the parent fiber. */
	sCurrent = prev;
	/* Back in the resumer's context — if that is the main fiber this is
	 * the earliest point where queued fiber logs can be persisted
	 * safely (no coroutine running => locks legal again). */
	port_log_fiber_drain();
}

void port_coroutine_yield(void)
{
	PortCoroutine *co = sCurrent;

	if (co == NULL) {
		return;
	}
	port_watchdog_note_yield();
	sCurrent = co->caller_co;       /* resumer's identity (NULL if main fiber) */
	emscripten_fiber_swap(&co->fiber, co->caller);
	stack_canary_check(co, "yield-return");
	sCurrent = co;                  /* resumed again — we are current once more */
}

int port_coroutine_is_finished(PortCoroutine *co)
{
	return coro::is_finished(co);
}

int port_coroutine_in_coroutine(void)
{
	return coro::in_coroutine(sCurrent);
}

uint32_t port_coroutine_serial_now(void)
{
	return sNextSerial;
}

void port_coroutine_reap_after(uint32_t serial)
{
	coro::reap_after(serial, sLive, sCurrent);
}

#endif /* __EMSCRIPTEN__ */

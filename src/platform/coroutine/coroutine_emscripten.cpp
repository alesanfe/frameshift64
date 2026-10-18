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
#include "port_watchdog.h"

#include <emscripten/fiber.h>
#include <stdlib.h>
#include <string.h>

struct PortCoroutine {
	emscripten_fiber_t fiber;
	emscripten_fiber_t *caller;     /* fiber that resumed us — yield target */
	void *c_stack;                  /* fiber's real call stack (malloc'd) */
	void *asyncify_stack;           /* fiber's asyncify stack (malloc'd) */
	size_t asyncify_size;
	void (*entry)(void *);          /* entry point */
	void *arg;
	int finished;                   /* entry returned */
	PortCoroutine *next_live;       /* registry link (rollback reap) */
	uint32_t serial;                /* creation order */
};

static PortCoroutine *sCurrent;     /* running fiber, NULL on main thread */
static emscripten_fiber_t sMainFiber;
static int sMainReady;
static uint32_t sNextSerial;
static PortCoroutine *sLive;        /* intrusive list, newest first */

#define ASYNCIFY_STACK_BYTES (128 * 1024)
#define MAIN_ASYNCIFY_STACK_BYTES (64 * 1024)
static void *sMainAsyncifyStack;

static void coroutine_track(PortCoroutine *co)
{
	co->serial = ++sNextSerial;
	co->next_live = sLive;
	sLive = co;
}

static void coroutine_untrack(PortCoroutine *co)
{
	PortCoroutine **pp = &sLive;
	while (*pp != NULL) {
		if (*pp == co) {
			*pp = co->next_live;
			break;
		}
		pp = &(*pp)->next_live;
	}
	co->next_live = NULL;
}

static void coroutine_trampoline(void *arg)
{
	PortCoroutine *co = (PortCoroutine *)arg;

	co->entry(co->arg);
	co->finished = 1;
	sCurrent = NULL;
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
	co->c_stack = malloc(stack_size);
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
	emscripten_fiber_init(&co->fiber, coroutine_trampoline, co,
	                      co->c_stack, stack_size,
	                      co->asyncify_stack, co->asyncify_size);
	coroutine_track(co);
	return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
	if (co == NULL) {
		return;
	}
	coroutine_untrack(co);
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
	resumer = (sCurrent != NULL) ? &sCurrent->fiber : &sMainFiber;
	co->caller = resumer;
	sCurrent = co;
	emscripten_fiber_swap(resumer, &co->fiber);
	/* Control returns here when `co` yields or finishes. sCurrent is cleared
	 * by whichever side handed control back (yield sets it via the caller's
	 * own resume path; the trampoline clears it on finish). */
	if (sCurrent == co) {
		sCurrent = NULL;
	}
}

void port_coroutine_yield(void)
{
	PortCoroutine *co = sCurrent;

	if (co == NULL) {
		return;
	}
	port_watchdog_note_yield();
	sCurrent = NULL;                /* resumer regains "not in coroutine" view */
	emscripten_fiber_swap(&co->fiber, co->caller);
	sCurrent = co;                  /* resumed again — we are current once more */
}

int port_coroutine_is_finished(PortCoroutine *co)
{
	return (co == NULL) || co->finished;
}

int port_coroutine_in_coroutine(void)
{
	return sCurrent != NULL;
}

uint32_t port_coroutine_serial_now(void)
{
	return sNextSerial;
}

void port_coroutine_reap_after(uint32_t serial)
{
	PortCoroutine *co = sLive;
	while (co != NULL) {
		PortCoroutine *next = co->next_live;
		if (co->serial > serial && co != sCurrent) {
			port_coroutine_destroy(co);
		}
		co = next;
	}
}

#endif /* __EMSCRIPTEN__ */

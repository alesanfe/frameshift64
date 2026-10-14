/**
 * coroutine_win32.cpp — Win32 Fiber-based coroutine implementation.
 *
 * Each coroutine is a Win32 Fiber with its own stack. The main thread
 * is converted to a fiber via ConvertThreadToFiber so it can participate
 * in SwitchToFiber calls. Resume/yield are symmetric SwitchToFiber calls.
 *
 * Stack canaries (0xDEADBEEF / 0xCAFEBABE) are used to detect corruption.
 */

#ifdef _WIN32

#include "coroutine.h"
#include "port_watchdog.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANARY_HEAD 0xDEADBEEFu
#define CANARY_TAIL 0xCAFEBABEu
#define MIN_STACK_SIZE 16384

struct PortCoroutine {
	unsigned int canary_head;
	void *fiber;          /* Win32 Fiber handle */
	void *caller_fiber;   /* Fiber that most recently resumed this coroutine */
	void (*entry)(void *);
	void *arg;
	int finished;
	void *stack_low;      /* Fiber stack bottom (DeallocationStack), for watermark check */
	int stack_low_warned; /* low-stack warning already emitted */
	PortCoroutine *next_live; /* registry link (AUD-27 rollback reap) */
	uint32_t serial;
	unsigned int canary_tail;
};

/* The main thread's fiber handle, set by port_coroutine_init_main. */
static void *sMainFiber = NULL;

/* The coroutine currently being executed (NULL if on main fiber). */
static thread_local PortCoroutine *sCurrentCoroutine = NULL;

/* Live-coroutine registry + creation serial. A rollback snapshot restores
 * the OSThread structs — the only owners of PortCoroutine pointers — to
 * pre-spawn state, so coroutines created during resimulation would leak
 * (Fiber handle + 64KB stack each). syNetRollbackLoad reaps them via
 * port_coroutine_reap_after(state->coroutine_serial). */
static PortCoroutine *sLiveCoroutines = NULL;
static uint32_t sNextCoroutineSerial = 0;

static void coroutine_track(PortCoroutine *co)
{
	co->serial = ++sNextCoroutineSerial;
	co->next_live = sLiveCoroutines;
	sLiveCoroutines = co;
}

static void coroutine_untrack(PortCoroutine *co)
{
	PortCoroutine **pp = &sLiveCoroutines;
	while (*pp != NULL) {
		if (*pp == co) {
			*pp = co->next_live;
			break;
		}
		pp = &(*pp)->next_live;
	}
	co->next_live = NULL;
}

uint32_t port_coroutine_serial_now(void)
{
	return sNextCoroutineSerial;
}

void port_coroutine_reap_after(uint32_t serial)
{
	PortCoroutine *co = sLiveCoroutines;
	while (co != NULL) {
		PortCoroutine *next = co->next_live;
		if (co->serial > serial && co != sCurrentCoroutine) {
			port_coroutine_destroy(co);
		}
		co = next;
	}
}

/* ========================================================================= */
/*  Internal: fiber entry point wrapper                                      */
/* ========================================================================= */

static void CALLBACK fiber_entry(void *param)
{
	PortCoroutine *co = (PortCoroutine *)param;

	/* Capture this fiber's stack bottom for the low-stack watermark check
	 * in port_coroutine_yield — replaces the inert N64 stack canary.
	 * GetCurrentThreadStackLimits (Win8+) covers fibers; the old
	 * NT_TIB.DeallocationStack field no longer exists in the 10.0.26100
	 * SDK headers. */
	ULONG_PTR stackLow = 0, stackHigh = 0;
	GetCurrentThreadStackLimits(&stackLow, &stackHigh);
	co->stack_low = (void *)stackLow;

	/* Run the user's entry function. */
	co->entry(co->arg);

	/* Entry returned — mark finished and switch back to caller. */
	co->finished = 1;
	sCurrentCoroutine = NULL;

	if (co->caller_fiber != NULL) {
		SwitchToFiber(co->caller_fiber);
	} else {
		/* Should never happen — if it does, switch to main. */
		SwitchToFiber(sMainFiber);
	}

	/* Should be unreachable. If we somehow get here, just spin
	 * (a finished coroutine should never be resumed). */
	for (;;) {
		SwitchToFiber(sMainFiber);
	}
}

/* ========================================================================= */
/*  Internal: validation                                                     */
/* ========================================================================= */

static int validate_coroutine(PortCoroutine *co, const char *caller)
{
	if (co == NULL) {
		fprintf(stderr, "SSB64 coroutine [%s]: NULL coroutine\n", caller);
		return 0;
	}
	if (co->canary_head != CANARY_HEAD || co->canary_tail != CANARY_TAIL) {
		fprintf(stderr, "SSB64 coroutine [%s]: memory corruption detected "
		        "(head=0x%08X tail=0x%08X)\n",
		        caller, co->canary_head, co->canary_tail);
		return 0;
	}
	return 1;
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void port_coroutine_init_main(void)
{
	if (sMainFiber != NULL) {
		return; /* already initialized */
	}
	sMainFiber = ConvertThreadToFiber(NULL);
	if (sMainFiber == NULL) {
		/* ConvertThreadToFiber fails if already a fiber (returns NULL with
		 * ERROR_ALREADY_FIBER). In that case, GetCurrentFiber works. */
		if (GetLastError() == ERROR_ALREADY_FIBER) {
			sMainFiber = GetCurrentFiber();
		}
		if (sMainFiber == NULL) {
			fprintf(stderr, "SSB64: ConvertThreadToFiber failed (error %lu)\n",
			        GetLastError());
			abort();
		}
	}
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg,
                                     size_t stack_size)
{
	PortCoroutine *co;

	if (sMainFiber == NULL) {
		fprintf(stderr, "SSB64: port_coroutine_create called before init_main\n");
		return NULL;
	}

	if (stack_size < MIN_STACK_SIZE) {
		stack_size = MIN_STACK_SIZE;
	}

	co = (PortCoroutine *)calloc(1, sizeof(PortCoroutine));
	if (co == NULL) {
		return NULL;
	}

	co->canary_head = CANARY_HEAD;
	co->canary_tail = CANARY_TAIL;
	co->entry = entry;
	co->arg = arg;
	co->finished = 0;
	co->caller_fiber = NULL;

	co->fiber = CreateFiber(stack_size, fiber_entry, co);
	if (co->fiber == NULL) {
		fprintf(stderr, "SSB64: CreateFiber failed (error %lu, stack=%zu)\n",
		        GetLastError(), stack_size);
		free(co);
		return NULL;
	}

	coroutine_track(co);
	return co;
}

void port_coroutine_destroy(PortCoroutine *co)
{
	if (co == NULL) {
		return;
	}
	if (!validate_coroutine(co, "destroy")) {
		return;
	}
	if (co == sCurrentCoroutine) {
		fprintf(stderr, "SSB64: port_coroutine_destroy on current coroutine\n");
		abort();
	}
	coroutine_untrack(co);
	if (co->fiber != NULL) {
		DeleteFiber(co->fiber);
		co->fiber = NULL;
	}
	/* Poison the struct to catch use-after-free. */
	co->canary_head = 0;
	co->canary_tail = 0;
	free(co);
}

void port_coroutine_resume(PortCoroutine *co)
{
	if (!validate_coroutine(co, "resume")) {
		return;
	}
	if (co->finished) {
		fprintf(stderr, "SSB64: attempted to resume finished coroutine\n");
		return;
	}
	if (co->fiber == NULL) {
		fprintf(stderr, "SSB64: attempted to resume coroutine with NULL fiber\n");
		return;
	}

	/* Save the current coroutine so nested resumes restore correctly.
	 * Example: main resumes Thread5, Thread5 resumes a GObj coroutine.
	 * When the GObj coroutine yields, sCurrentCoroutine must be restored
	 * to Thread5 (not NULL) so Thread5 can still yield later. */
	PortCoroutine *prev = sCurrentCoroutine;

	/* Record caller so yield knows where to return. */
	co->caller_fiber = GetCurrentFiber();
	sCurrentCoroutine = co;

	SwitchToFiber(co->fiber);

	/* We return here when the coroutine yields or finishes.
	 * Restore the previous coroutine context for the caller. */
	sCurrentCoroutine = prev;
}

void port_coroutine_yield(void)
{
	PortCoroutine *co = sCurrentCoroutine;

	if (co == NULL) {
		fprintf(stderr, "SSB64: port_coroutine_yield called outside coroutine\n");
		return;
	}

	port_watchdog_note_yield();

	/* Host stack watermark check — the N64 gobjthread->stack[7] canary is
	 * inert in the port (game code runs on this fiber's host stack). The
	 * OS still hard-faults on a real overrun via the fiber's guard page;
	 * this logs an earlier diagnostic while the process is still sane. */
	if (co->stack_low != NULL && !co->stack_low_warned) {
		volatile char probe;
		SIZE_T remaining = (SIZE_T)((char *)&probe - (char *)co->stack_low);
		if (remaining < 8192) {
			co->stack_low_warned = 1;
			fprintf(stderr, "SSB64 coroutine: stack nearly exhausted "
			        "(only %lu bytes left — grow PORT_STACK_* allocation)\n",
			        (unsigned long)remaining);
		}
	}

	sCurrentCoroutine = NULL;

	if (co->caller_fiber != NULL) {
		SwitchToFiber(co->caller_fiber);
	} else {
		SwitchToFiber(sMainFiber);
	}

	/* We return here when resumed. sCurrentCoroutine is restored by resume. */
}

int port_coroutine_is_finished(PortCoroutine *co)
{
	if (co == NULL) {
		return 1;
	}
	return co->finished;
}

int port_coroutine_in_coroutine(void)
{
	return sCurrentCoroutine != NULL;
}

#endif /* _WIN32 */

#pragma once

/* Shared live-list bookkeeping for the per-platform coroutine backends.
 * Every backend maintains an intrusive singly-linked list of its own
 * PortCoroutine struct (fields: serial, next_live) plus a creation-serial
 * counter and a "current" pointer — the walk logic is identical
 * everywhere, only the globals differ, so they're passed by reference.
 * See coroutine.h for the public surface each backend implements. */

#include "coroutine.h"

#include <stdint.h>

/* Stack-corruption canary words — backends that manage their own stack
 * (win32 fibers, Switch fake stack) stamp these at both ends; posix and
 * android use guard pages and emscripten a wider magic array instead. */
#ifndef CANARY_HEAD
#define CANARY_HEAD 0xDEADBEEFu
#endif
#ifndef CANARY_TAIL
#define CANARY_TAIL 0xCAFEBABEu
#endif

/* Floor for caller-requested stack sizes. Define MIN_STACK_SIZE before
 * including this header to pick a platform floor (win32 fibers commit
 * their own reservation, so they get a lower one). */
#ifndef MIN_STACK_SIZE
#define MIN_STACK_SIZE 32768
#endif

namespace coro {

template <typename Co>
inline void track(Co* co, Co*& live, uint32_t& serial)
{
	co->serial = ++serial;
	co->next_live = live;
	live = co;
}

template <typename Co>
inline void untrack(Co* co, Co*& live)
{
	Co** pp = &live;
	while (*pp != nullptr) {
		if (*pp == co) {
			*pp = co->next_live;
			break;
		}
		pp = &(*pp)->next_live;
	}
	co->next_live = nullptr;
}

/* Destroy every live coroutine with serial > `serial`, except `current`
 * (port_coroutine_reap_after, AUD-27 rollback reap). */
template <typename Co>
inline void reap_after(uint32_t serial, Co* live, const Co* current)
{
	Co* co = live;
	while (co != nullptr) {
		Co* next = co->next_live;
		if (co->serial > serial && co != current) {
			port_coroutine_destroy(co);
		}
		co = next;
	}
}

} // namespace coro

/*
 * heap_check_report.cpp — part of the optional CRT debug-heap verification for Debug
 * MSVC test builds (see heap_check_boot.cpp for the full design notes).
 */
#include "heap_check_internal.h"
#if defined(_MSC_VER) && defined(_DEBUG) && defined(SSB64_TEST_HOOKS)

HANDLE gProbeLog = INVALID_HANDLE_VALUE;
char   gProbeBuf[1024];

void probeLog(const char* fmt, ...) {
	if (gProbeLog == INVALID_HANDLE_VALUE) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(gProbeBuf, sizeof(gProbeBuf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		DWORD written = 0;
		// WriteFile on a plain handle is already unbuffered — FlushFileBuffers
		// per message cost the earlier runs minutes of syscall traffic.
		WriteFile(gProbeLog, gProbeBuf,
		          (DWORD)(n < (int)sizeof(gProbeBuf) ? n : (int)sizeof(gProbeBuf) - 1),
		          &written, nullptr);
	}
}

bool addrLooksMapped(const void* p) {
	const uintptr_t a = (uintptr_t)p;
	if (a < 0x10000 || a >= 0x0000800000000000ull) {
		return false;
	}
	/* Range check alone is not enough: a list link can hold a pointer into
	 * a heap segment that was committed once but released back to the OS
	 * after the block was freed (decommitted page = valid address, MEM_FREE
	 * state). A later unlink then writes through it -> AV inside free_dbg.
	 * VirtualQuery proves the page is actually committed. */
	MEMORY_BASIC_INFORMATION mbi;
	return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	       mbi.State == MEM_COMMIT &&
	       (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
	                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
	                       PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Resolve a pointer to "module+0xoff" when it lands inside a loaded image —
// the stomped link values observed so far point into module space (a
// transient driver DLL region), so naming the module identifies the writer.
void describePtr(const void* p, char* out, size_t outSize) {
	const uintptr_t a = (uintptr_t)p;
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) == sizeof(mbi) &&
	    mbi.Type == MEM_IMAGE && mbi.AllocationBase != nullptr) {
		char name[MAX_PATH] = {0};
		GetModuleFileNameA((HMODULE)mbi.AllocationBase, name, sizeof(name) - 1);
		const char* base = name;
		for (const char* q = name; *q; ++q) {
			if (*q == '\\' || *q == '/') {
				base = q + 1;
			}
		}
		snprintf(out, outSize, "%s+0x%llx", base,
		         (unsigned long long)(a - (uintptr_t)mbi.AllocationBase));
	} else {
		snprintf(out, outSize, "%p (not image%s)", p,
		         (mbi.State == MEM_COMMIT) ? ", committed" : ", unmapped");
	}
}

// Bounded, SEH-guarded render of h->file_name — the field may itself be
// stomped, so it is dereferenced only after a plausibility check.
static void describeVictimFileName(const ShadowCrtHeader* h) {
	if (addrLooksMapped(h->file_name)) {
		__try {
			char nameBuf[128];
			size_t i = 0;
			for (; i < sizeof(nameBuf) - 1 && h->file_name[i] != '\0'; ++i) {
				nameBuf[i] = h->file_name[i];
			}
			nameBuf[i] = '\0';
			probeLog("    alloc site: %s:%d\n", nameBuf, h->line_number);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			probeLog("    alloc site: (file_name %p unreadable)\n", (const void*)h->file_name);
		}
	}
}

static void describeVictimHeaderBytes(const ShadowCrtHeader* h) {
	__try {
		const unsigned char* b = (const unsigned char*)h;
		probeLog("    header bytes:");
		for (int i = 0; i < 64; ++i) {
			if ((i % 16) == 0) {
				probeLog("\n    %04x:", i);
			}
			probeLog(" %02x", b[i]);
		}
		probeLog("\n");
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		probeLog("    (header partially unreadable)\n");
	}
}

/* When next->prev points elsewhere, that "skip" node is the competing
 * claimant for the list position — inspect whether it is still alive
 * (use/gaps intact) or freed (0xDD fill). A live skip node means h->next
 * was overwritten by a wild pointer store; a dead one means an unlink
 * replayed stale links (double-free / free of an already-removed node). */
static void describeVictimSkipNode(const ShadowCrtHeader* h) {
	const ShadowCrtHeader* skip = h->next->prev;
	if (skip == h || !addrLooksMapped(skip)) {
		return;
	}
	probeLog("    skip-node %p: next=%p prev=%p file=%p line=%d "
	         "use=%d size=%llu req=%ld\n",
	         (const void*)skip, (const void*)skip->next,
	         (const void*)skip->prev, (const void*)skip->file_name,
	         skip->line_number, skip->block_use,
	         (unsigned long long)skip->data_size, skip->request_number);
	if (addrLooksMapped(skip->file_name)) {
		char nb[128];
		size_t i = 0;
		for (; i < sizeof(nb) - 1 && skip->file_name[i] != '\0'; ++i) {
			nb[i] = skip->file_name[i];
		}
		nb[i] = '\0';
		probeLog("    skip-node alloc site: %s:%d\n", nb, skip->line_number);
	}
}

/* Cross-link truth: for a "next->prev broken"/"prev->next broken"
 * report the value the neighbor ACTUALLY holds — that is the stomp
 * payload (a foreign pointer, -1 fill, dead-land bytes) and often
 * identifies the writer. */
static void describeVictimNeighbors(const ShadowCrtHeader* h) {
	__try {
		if (addrLooksMapped(h->next)) {
			probeLog("    next(%p)->prev = %p%s\n", (const void*)h->next,
			         (const void*)h->next->prev,
			         h->next->prev == h ? " (consistent)" : "  <-- mismatch");
			if (h->next->prev != h) {
				char nb[160];
				describePtr(h->next->prev, nb, sizeof(nb));
				probeLog("    (mismatched prev resolves to %s)\n", nb);
				/* Also dump the next node's own identity — is it a real
				 * live block (=> its prev was stomped) or garbage memory
				 * (=> victim.next itself was stomped). */
				const ShadowCrtHeader* nn = h->next;
				probeLog("    next-node %p: next=%p prev=%p file=%p line=%d "
				         "use=%d size=%llu req=%ld\n",
				         (const void*)nn, (const void*)nn->next,
				         (const void*)nn->prev, (const void*)nn->file_name,
				         nn->line_number, nn->block_use,
				         (unsigned long long)nn->data_size, nn->request_number);
			}
			describeVictimSkipNode(h);
		}
		if (addrLooksMapped(h->prev)) {
			probeLog("    prev(%p)->next = %p%s\n", (const void*)h->prev,
			         (const void*)h->prev->next,
			         h->prev->next == h ? " (consistent)" : "  <-- mismatch");
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		probeLog("    (neighbor links unreadable)\n");
	}
}

// Print the victim header's fields numerically — _file_name may itself be
// stomped, so it is rendered as a pointer and only dereferenced (bounded,
// SEH-guarded) after a plausibility check.
void describeVictim(const ShadowCrtHeader* h, const char* why, const char* tag) {
	probeLog("*** HEAP-AUTOPSY %s [%s]: header=%p user=%p next=%p prev=%p "
	         "file_name=%p line=%d use=%d size=%llu req=%ld ***\n",
	         why, tag, (const void*)h, (const void*)(h + 1),
	         (const void*)h->next, (const void*)h->prev,
	         (const void*)h->file_name, h->line_number, h->block_use,
	         (unsigned long long)h->data_size, h->request_number);
	describeVictimFileName(h);
	describeVictimHeaderBytes(h);
	describeVictimNeighbors(h);
}



// Cheap plausibility for pointers: no VirtualQuery — a dereference of a
// non-committed address is caught by the caller's __try anyway, and this
// runs per free, so syscall-per-field is the difference between seconds
// and minutes during resource loading.
bool addrPlausible(const void* p) {
	const uintptr_t a = (uintptr_t)p;
	return a >= 0x10000 && a < 0x0000800000000000ull;
}

// List-link integrity: next->prev and prev->next must point back at h.
static const char* checkHeaderLinks(const ShadowCrtHeader* h) {
	if (h->next != nullptr) {
		if (!addrPlausible(h->next)) {
			return "next implausible";
		}
		if (h->next->prev != h) {
			return "next->prev broken";
		}
	}
	if (h->prev != nullptr) {
		if (!addrPlausible(h->prev)) {
			return "prev implausible";
		}
		if (h->prev->next != h) {
			return "prev->next broken";
		}
	}
	return nullptr;
}

/* file_name/line/request live in the same header as the links — a wild
 * write that hits only those fields (observed: file_name stomped to -1,
 * which then kills _CrtDbgReport's strnlen) leaves links+gaps intact and
 * would pass every check above. Validate the pointer too. */
static const char* checkHeaderMeta(const ShadowCrtHeader* h) {
	if (h->file_name != nullptr && !addrPlausible(h->file_name)) {
		return "file_name implausible";
	}
	if (h->file_name == nullptr && h->line_number != 0) {
		return "line set but file null";
	}
	return nullptr;
}

static const char* checkHeaderGaps(const ShadowCrtHeader* h) {
	for (int i = 0; i < 4; ++i) {
		if (h->gap[i] != 0xFD) {
			return "head gap";
		}
	}
	if (h->data_size <= 0x10000000ull) {
		const unsigned char* tail = (const unsigned char*)(h + 1) + h->data_size;
		for (int i = 0; i < 4; ++i) {
			if (tail[i] != 0xFD) {
				return "tail gap";
			}
		}
	}
	return nullptr;
}

// Returns why-string if the header's list links or gaps are inconsistent.
// All dereferences run under the caller's __try — a faulted read means the
// header itself is the damage; the caller reports it.
const char* checkHeader(const ShadowCrtHeader* h) {
	if (!addrPlausible(h)) {
		return "header ptr implausible";
	}
	if (h->block_use != 1 && h->block_use != 2 && h->block_use != 4) {
		return "bad block_use";
	}
	const char* why = checkHeaderLinks(h);
	if (why == nullptr) {
		why = checkHeaderMeta(h);
	}
	if (why == nullptr) {
		why = checkHeaderGaps(h);
	}
	return why;
}




/* Address of the exe's own __acrt_first_block static, found by scanning
 * the exe image (no PDB dependency at runtime). Resolved lazily on the
 * first head-candidate free. */
uintptr_t gExeAnchorAddr = 0;
bool      gExeAnchorTried = false;

const ShadowCrtHeader* exeListHead() {
	if (!gExeAnchorTried) {
		gExeAnchorTried = true;
		gExeAnchorAddr = findListHeadGlobal(GetModuleHandleW(nullptr));
		if (gExeAnchorAddr != 0) {
			probeLog("heap-hook: exe __acrt_first_block global @ %p\n",
			         (const void*)gExeAnchorAddr);
		}
	}
	if (gExeAnchorAddr == 0) {
		return nullptr;
	}
	const ShadowCrtHeader* h = nullptr;
	__try {
		h = *(ShadowCrtHeader* const*)gExeAnchorAddr;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return h;
}

#endif

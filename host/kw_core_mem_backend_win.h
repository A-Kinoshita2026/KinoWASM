/* kw_core_mem_backend_win.h - Windows backend for the core linear memory (host layer).
 *
 * The library (kw_core_bridge.c) contains no platform-specific code; the host injects the
 * memory backend at init via kw_core_set_mem_backend(). This header provides the Windows
 * implementation (VirtualAlloc reserve/commit) and the installer kw_core_install_default_mem_backend().
 *
 * Usage: at the top of the host's main, call
 *     #include "kw_core_mem_backend_win.h"
 *     kw_core_install_default_mem_backend();
 * To port to iOS/Android/consoles, supply a different header that builds the same
 * kw_core_mem_backend_t via mmap(PROT_NONE)+mprotect (or the platform VM API) and swap the
 * include - the library stays untouched.
 *
 * NOTE: ASCII-only (no UTF-8 comments) so MSVC under CP932 + /WX does not raise C4819.
 */
#ifndef KW_CORE_MEM_BACKEND_WIN_H
#define KW_CORE_MEM_BACKEND_WIN_H

#include "core/kw_core.h"
#include <windows.h>

/* Reserve address space (no physical pages) + commit the initial bytes. Returns base or NULL. */
static void* kw_core_win_mem_reserve(uint64_t reserve_bytes, uint64_t commit_bytes)
{
	uint8_t* base = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)reserve_bytes, MEM_RESERVE, PAGE_READWRITE);
	if (base == NULL) return NULL;
	if (commit_bytes > 0 && VirtualAlloc(base, (SIZE_T)commit_bytes, MEM_COMMIT, PAGE_READWRITE) == NULL) {
		VirtualFree(base, 0, MEM_RELEASE);
		return NULL;
	}
	return base;
}

/* grow: commit [offset, offset+len) within the reserved range. Returns 0 on success, -1 on failure. */
static int kw_core_win_mem_commit(void* base, uint64_t offset, uint64_t len)
{
	return VirtualAlloc((uint8_t*)base + offset, (SIZE_T)len, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

static void kw_core_win_mem_release(void* base)
{
	VirtualFree(base, 0, MEM_RELEASE);
}

static const kw_core_mem_backend_t kw_core_win_mem_backend = {
	kw_core_win_mem_reserve, kw_core_win_mem_commit, kw_core_win_mem_release
};

/* Host calls this at startup to drive core linear memory via the Windows VirtualAlloc backend. */
static void kw_core_install_default_mem_backend(void)
{
	kw_core_set_mem_backend(&kw_core_win_mem_backend);
}

#endif /* KW_CORE_MEM_BACKEND_WIN_H */

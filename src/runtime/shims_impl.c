/*
 * MechCommander Gold recompilation - hand-written import shims.
 *
 * Everything here runs inside the simulated machine: arguments come off the
 * simulated stack via ARG(n), pointers are simulated VAs, and the epilogue pops
 * exactly what the real API pops. The STDRET counts are NOT typed from memory --
 * gen_imports.py derives each one and verifies this file against it on every
 * run, so a wrong count is a build error rather than a silent stack desync.
 *
 * Two principles the house has paid for:
 *
 *   - A stub that returns 0 is not neutral. MMSYSERR_NOERROR, DS_OK and DD_OK
 *     are all zero, so returning 0 claims success and leaves the caller's out
 *     struct uninitialised. Anything unimplemented here reports ABSENT.
 *   - File I/O and time are forwarded to the host's own Win32, because the host
 *     IS Windows. The only translation needed is the handle: a host HANDLE is
 *     64-bit and the simulated one is a 32-bit slot, so open files and finds
 *     live in tagged tables. Buffers need no translation -- simulated memory is
 *     host memory, so ReadFile writes straight into the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"

/* ============================================================
 * The arena: all simulated dynamic memory
 * ============================================================ */

#define ARENA_SIZE  0x10000000u   /* 256 MB reserved, committed on demand */
#define PAGE_SZ     0x1000u

static uint32_t g_arena_base = 0, g_arena_next = 0, g_arena_end = 0;
static uint32_t g_arena_committed = 0;

static uint32_t page_up(uint32_t n) { return (n + PAGE_SZ - 1) & ~(PAGE_SZ - 1); }

int shims_init(uint32_t base) {
    base = (base + 0xFFFFu) & ~0xFFFFu;          /* allocation granularity */
    if (!VirtualAlloc((void*)(uintptr_t)base, ARENA_SIZE, MEM_RESERVE,
                      PAGE_READWRITE)) {
        fprintf(stderr, "[shims] arena reserve @ 0x%08X (%u MB) failed (%lu)\n",
                base, ARENA_SIZE / 1048576u, GetLastError());
        return 0;
    }
    g_arena_base = g_arena_next = g_arena_committed = base;
    g_arena_end = base + ARENA_SIZE;
    fprintf(stderr, "[shims] heap arena 0x%08X-0x%08X\n", g_arena_base, g_arena_end);
    return 1;
}

/* Printed with every crash report. Bring-up is mostly a hunt for one value,
 * and having the arena's real state in the report turns a build-and-run per
 * hypothesis into a single run. */
static uint32_t g_alloc_count = 0, g_alloc_largest = 0, g_alloc_total = 0;

void shims_report(void) {
    fprintf(stderr, "  arena: base=%08X next=%08X committed=%08X end=%08X\n",
            g_arena_base, g_arena_next, g_arena_committed, g_arena_end);
    fprintf(stderr, "         %u allocations, %u bytes total, largest %u\n",
            g_alloc_count, g_alloc_total, g_alloc_largest);
}

uint32_t shims_arena_base(void) { return g_arena_base; }
uint32_t shims_arena_end(void)  { return g_arena_end; }

static uint32_t arena_commit_to(uint32_t want_end) {
    if (want_end <= g_arena_committed) return 1;
    uint32_t end = page_up(want_end);
    if (end > g_arena_end) return 0;
    if (!VirtualAlloc((void*)(uintptr_t)g_arena_committed,
                      end - g_arena_committed, MEM_COMMIT, PAGE_READWRITE))
        return 0;
    memset((void*)ADDR(g_arena_committed), 0, end - g_arena_committed);
    g_arena_committed = end;
    return 1;
}

/*
 * ponytail: bump allocator with an 8-byte size header; free is a no-op.
 * MechCommander takes its pools at startup and holds them, so nothing here
 * fragments during bring-up, and HeapSize/HeapReAlloc stay correct because the
 * size is recorded. The ceiling is a long session exhausting 256 MB -- put a
 * real free list behind arena_alloc if that ever happens.
 */
static uint32_t arena_alloc(uint32_t size) {
    uint32_t need = (size + 15u) & ~15u;
    uint32_t hdr = g_arena_next;
    if (hdr + need + 16u >= g_arena_end) return 0;
    if (!arena_commit_to(hdr + need + 16u)) return 0;
    g_arena_next = hdr + 16u + need;
    g_alloc_count++;
    g_alloc_total += size;
    if (size > g_alloc_largest) g_alloc_largest = size;
    MEM32(hdr) = size;
    MEM32(hdr + 4) = 0xA110C8EDu;          /* so a bad free is visible */
    return hdr + 16u;
}

static uint32_t arena_size_of(uint32_t p) {
    if (p < g_arena_base + 16u || p >= g_arena_next) return 0;
    if (MEM32(p - 12) != 0xA110C8EDu) return 0;
    return MEM32(p - 16);
}

/* ============================================================
 * The simulated TIB
 *
 * `fs:` is thread-relative on Win32 and the lifter knows it -- it emits
 * FS_BASE + offset for every fs: access. The runtime just has to point
 * g_fs_base at a block shaped like a TIB. Leave it 0 and fs:[0] reads address
 * zero, which is a null dereference during CRT startup.
 *
 * fs:[0] is the head of the SEH chain; 0xFFFFFFFF is the real end-of-chain
 * sentinel. We never dispatch SEH -- exceptions go to the host's handler -- so
 * the chain is only ever written and walked by the program itself.
 * ============================================================ */

#define TIB_EXCEPTION_LIST  0x00
#define TIB_STACK_BASE      0x04   /* high address: the top of the stack */
#define TIB_STACK_LIMIT     0x08   /* low address: the bottom */
#define TIB_SELF            0x18
#define TIB_PID             0x20
#define TIB_TID             0x24
#define TIB_SIZE            0x1000

uint32_t shims_make_tib(uint32_t stack_base, uint32_t stack_top) {
    uint32_t tib = arena_alloc(TIB_SIZE);
    if (!tib) return 0;
    memset((void*)ADDR(tib), 0, TIB_SIZE);
    MEM32(tib + TIB_EXCEPTION_LIST) = 0xFFFFFFFFu;
    MEM32(tib + TIB_STACK_BASE)     = stack_top;
    MEM32(tib + TIB_STACK_LIMIT)    = stack_base;
    MEM32(tib + TIB_SELF)           = tib;
    MEM32(tib + TIB_PID)            = 0x2000;
    MEM32(tib + TIB_TID)            = 0x1000;
    fprintf(stderr, "[shims] TIB at 0x%08X\n", tib);
    return tib;
}

/* ============================================================
 * Copying values into simulated memory
 * ============================================================ */

static uint32_t sim_put(const void* src, uint32_t n) {
    uint32_t p = arena_alloc(n);
    if (p) memcpy((void*)ADDR(p), src, n);
    return p;
}

static uint32_t sim_put_str(const char* s) {
    return sim_put(s, (uint32_t)strlen(s) + 1);
}

/* Same thing, for the CRT self-test in the runtime. */
uint32_t shims_put_str(const char* s) { return sim_put_str(s); }

/* Copy into a caller-provided buffer, returning what the Win32 convention
 * wants: the length written, excluding the terminator. */
static uint32_t sim_copy_out(uint32_t buf, uint32_t cap, const char* s) {
    uint32_t n = (uint32_t)strlen(s);
    if (!buf || !cap) return n;
    if (n >= cap) n = cap - 1;
    memcpy((void*)ADDR(buf), s, n);
    MEM8(buf + n) = 0;
    return n;
}

/* ============================================================
 * Program identity
 * ============================================================ */

static char g_program[MAX_PATH] = "MCX.EXE";
static uint32_t g_cmdline_va = 0;
static uint32_t g_env_va = 0;
static uint32_t g_last_error = 0;

/* MCX.EXE loaded as a plain datafile, purely so the host's own resource loader
 * can answer LoadStringA out of the game's .rsrc. No code is mapped or run --
 * LOAD_LIBRARY_AS_DATAFILE only makes the resources readable. */
static HMODULE g_resources;

static char g_install_dir[MAX_PATH];

/*
 * Root-anchored paths resolve against the install directory.
 *
 * checkForCDInDrive() probes `\data\tiles\gtiles90.pak` and `\hidden.txt` --
 * paths built from a drive ROOT, because in 1999 the content was on a CD whose
 * root *was* the data directory. Installed into a subfolder, those can never
 * resolve, and the engine stops at "Please insert the MechCommander Expansion
 * compact disc" no matter what is actually on the machine.
 *
 * So a path starting at a root is retried under the install directory, and only
 * when the result actually exists; anything else passes through untouched. This
 * answers "where does my content live", which is all the check is asking --
 * MCX.EXE carries no DRM of any kind (Phase 0 found no packer and no protection
 * indicators), and both files it wants are the game's own, sitting right there.
 */
static const char* redirect(const char* name, char* buf, size_t cap) {
    const char* rel = NULL;
    if (!name || !g_install_dir[0])
        return name;
    if (name[0] == '\\' || name[0] == '/')
        rel = name + 1;
    else if (name[0] && name[1] == ':' && (name[2] == '\\' || name[2] == '/'))
        rel = name + 3;
    if (!rel)
        return name;
    snprintf(buf, cap, "%s%s", g_install_dir, rel);
    return GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES ? buf : name;
}

void shims_set_program(const char* path) {
    GetFullPathNameA(path, MAX_PATH, g_program, NULL);
    snprintf(g_install_dir, sizeof g_install_dir, "%s", g_program);
    for (char* p = g_install_dir + strlen(g_install_dir); p > g_install_dir; p--)
        if (p[-1] == '\\' || p[-1] == '/') { *p = 0; break; }
    g_resources = LoadLibraryExA(g_program, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!g_resources)
        fprintf(stderr, "[shims] no resources from %s (%lu)\n",
                g_program, GetLastError());
}

/* The engine loads every message it shows through this. Stubbed, it returned 0
 * -- a valid "string not found" -- so the game put up an empty dialog, and then
 * did it again 43,000 times. */
void imp_LoadStringA(void) {
    char tmp[1024];
    int n = g_resources ? LoadStringA(g_resources, ARG(1), tmp, sizeof tmp) : 0;
    if (n > 0 && ARG(2) && ARG(3))
        n = (int)sim_copy_out(ARG(2), ARG(3), tmp);
    else if (n <= 0 && ARG(2) && ARG(3))
        MEM8(ARG(2)) = 0;              /* never leave the buffer stale */
    RET(n > 0 ? (uint32_t)n : 0);
    STDRET(4);
}

void imp_GetCommandLineA(void) {
    if (!g_cmdline_va) {
        char line[MAX_PATH + 4];
        snprintf(line, sizeof line, "\"%s\"", g_program);
        g_cmdline_va = sim_put_str(line);
    }
    RET(g_cmdline_va);
    STDRET(0);
}

void imp_GetModuleHandleA(void) {
    /* NULL means "this module", which is the image base. A named module is one
     * we have not loaded; saying so is better than handing back the exe. */
    RET(ARG(0) ? 0 : recomp_image_base());
    STDRET(1);
}

void imp_GetModuleFileNameA(void) {
    RET(sim_copy_out(ARG(1), ARG(2), g_program));
    STDRET(3);
}

/* The CRT reads cb, dwFlags and cbReserved2 out of this. A zeroed 32-bit
 * STARTUPINFOA with cb set says "no inherited handles, no overrides", which is
 * true here -- and is not the same as leaving the struct untouched. */
void imp_GetStartupInfoA(void) {
    uint32_t p = ARG(0);
    if (p) {
        memset((void*)ADDR(p), 0, 68);
        MEM32(p) = 68;                 /* cb: the 32-bit struct size */
    }
    STDRET(1);
}

/* The host's real environment, copied into simulated memory.
 *
 * An empty-but-well-formed block parses fine, but it is not what the program
 * would have seen: the CRT builds environ from this, and the engine calls
 * getenv() during startup. Giving it the real thing removes a divergence
 * rather than leaving one that only shows up as a NULL somewhere later. */
void imp_GetEnvironmentStrings(void) {
    if (!g_env_va) {
        const char* env = GetEnvironmentStringsA();
        uint32_t n = 0;
        if (env) {                        /* walk to the double NUL */
            while (env[n] || env[n + 1]) n++;
            n += 2;
        }
        if (n) {
            g_env_va = sim_put(env, n);
        } else {
            static const char empty[2] = { 0, 0 };
            g_env_va = sim_put(empty, sizeof empty);
        }
        if (env) FreeEnvironmentStringsA((LPCH)env);
    }
    RET(g_env_va);
    STDRET(0);
}

void imp_GetEnvironmentStringsW(void) {
    static uint32_t wenv = 0;
    if (!wenv) {
        static const wchar_t empty[2] = { 0, 0 };
        wenv = sim_put(empty, sizeof empty);
    }
    RET(wenv);
    STDRET(0);
}

void imp_FreeEnvironmentStringsA(void) { RET(1); STDRET(1); }
void imp_FreeEnvironmentStringsW(void) { RET(1); STDRET(1); }

/* Windows 98 (4.10), which is what a 1999 build expects to see. The high bit
 * clear means "NT" to some checks, so report the 9x shape the game shipped on. */
void imp_GetVersion(void)        { RET(0xC0000A04u); STDRET(0); }
void imp_GetCurrentProcess(void) { RET(0xFFFFFFFFu); STDRET(0); }
void imp_GetLastError(void)      { RET(g_last_error); STDRET(0); }

void imp_OutputDebugStringA(void) {
    uint32_t s = ARG(0);
    if (s) fprintf(stderr, "[dbg] %s", (const char*)ADDR(s));
    STDRET(1);
}

/* ============================================================
 * Heap. The MSVC 6 CRT routes every malloc through HeapAlloc, so these must
 * return VAs inside the simulated address space, never host pointers.
 * ============================================================ */

#define MCG_HEAP_HANDLE  0x00BEEF01u

void imp_HeapCreate(void)     { RET(MCG_HEAP_HANDLE); STDRET(3); }
void imp_GetProcessHeap(void) { RET(MCG_HEAP_HANDLE); STDRET(0); }
void imp_HeapDestroy(void)    { RET(1); STDRET(1); }

void imp_HeapAlloc(void) {
    uint32_t flags = ARG(1), size = ARG(2);
    uint32_t p = arena_alloc(size ? size : 1);
    if (p && (flags & 8u))              /* HEAP_ZERO_MEMORY */
        memset((void*)ADDR(p), 0, size);
    if (!p) {
        fprintf(stderr, "[shims] HeapAlloc(%u) exhausted the arena\n", size);
        g_last_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    RET(p);
    STDRET(3);
}

void imp_HeapFree(void) { RET(1); STDRET(3); }   /* see arena_alloc's note */

void imp_HeapSize(void) {
    RET(arena_size_of(ARG(2)));
    STDRET(3);
}

void imp_HeapReAlloc(void) {
    uint32_t flags = ARG(1), old = ARG(2), size = ARG(3);
    uint32_t had = arena_size_of(old);
    uint32_t p = arena_alloc(size ? size : 1);
    if (p) {
        uint32_t n = had < size ? had : size;
        if (old && n) memcpy((void*)ADDR(p), (void*)ADDR(old), n);
        if ((flags & 8u) && size > n)
            memset((void*)ADDR(p + n), 0, size - n);
    }
    RET(p);
    STDRET(4);
}

/* ============================================================
 * Virtual memory
 * ============================================================ */

/*
 * VirtualAlloc has an alignment contract, and it is load-bearing.
 *
 * With lpAddress NULL the real API never returns anything but a multiple of the
 * 64 KB allocation granularity, and callers rely on that without checking. The
 * MSVC 6 small-block heap is one: __sbh_alloc_new_region reserves 1 MB and then
 * recovers the region and group from a block pointer by masking its low bits.
 * Handing it the 16-byte-aligned bump pointer arena_alloc returns made those
 * masks produce a base that was itself an absolute address, and the first write
 * through it landed 12 MB outside anything allocated.
 *
 * So this rounds the same way the real one does. A page-granular size, too:
 * a reservation is always whole pages.
 */
static uint32_t virt_alloc(uint32_t size) {
    uint32_t base = (g_arena_next + 0xFFFFu) & ~0xFFFFu;   /* 64 KB granularity */
    uint32_t need = page_up(size ? size : 1);
    if (base + need >= g_arena_end) return 0;
    if (!arena_commit_to(base + need)) return 0;
    g_arena_next = base + need;
    return base;
}

void imp_VirtualAlloc(void) {
    uint32_t want = ARG(0), size = ARG(1);
    /* A commit inside a region we already handed out is a no-op: the arena is
     * committed as it grows, so the memory is already there. */
    if (want && want >= g_arena_base && want < g_arena_end) {
        RET(arena_commit_to(want + size) ? want : 0);
        STDRET(4);
        return;
    }
    RET(virt_alloc(size));
    STDRET(4);
}

void imp_VirtualFree(void) { RET(1); STDRET(3); }

/* 32-bit MEMORY_BASIC_INFORMATION is seven DWORDs; the host's has 64-bit
 * pointers, so it is filled field by field rather than copied. */
void imp_VirtualQuery(void) {
    uint32_t addr = ARG(0), buf = ARG(1), len = ARG(2);
    if (!buf || len < 28) { RET(0); STDRET(3); return; }
    uint32_t base, end, protect = PAGE_READWRITE, state = MEM_COMMIT;
    if (addr >= recomp_image_base() && addr < recomp_image_end()) {
        base = recomp_image_base(); end = recomp_image_end();
    } else if (addr >= recomp_stack_base() && addr < recomp_stack_end()) {
        base = recomp_stack_base(); end = recomp_stack_end();
    } else if (addr >= g_arena_base && addr < g_arena_end) {
        base = g_arena_base; end = g_arena_end;
    } else {
        base = addr & ~0xFFFFu; end = base + 0x10000u;
        state = MEM_FREE; protect = PAGE_NOACCESS;
    }
    MEM32(buf +  0) = base;                 /* BaseAddress */
    MEM32(buf +  4) = base;                 /* AllocationBase */
    MEM32(buf +  8) = protect;              /* AllocationProtect */
    MEM32(buf + 12) = end - (addr & ~0xFFFu); /* RegionSize */
    MEM32(buf + 16) = state;
    MEM32(buf + 20) = protect;
    MEM32(buf + 24) = MEM_PRIVATE;
    RET(28);
    STDRET(3);
}

void imp_GlobalMemoryStatus(void) {
    uint32_t p = ARG(0);
    if (p) {
        MEMORYSTATUSEX host; host.dwLength = sizeof host;
        GlobalMemoryStatusEx(&host);
        /* Report what a 32-bit process can actually see, not the host's 32 GB:
         * the CRT and the engine both size pools from these numbers, and a
         * value that does not fit in 32 bits wraps to something absurd. */
        const uint32_t cap = 0x7FFFFFFFu;
        uint64_t phys = host.ullTotalPhys, avail = host.ullAvailPhys;
        MEM32(p +  0) = 32;                                  /* dwLength */
        MEM32(p +  4) = (uint32_t)host.dwMemoryLoad;
        MEM32(p +  8) = phys  > cap ? cap : (uint32_t)phys;
        MEM32(p + 12) = avail > cap ? cap : (uint32_t)avail;
        MEM32(p + 16) = cap;                                 /* TotalPageFile */
        MEM32(p + 20) = cap;                                 /* AvailPageFile */
        MEM32(p + 24) = cap;                                 /* TotalVirtual */
        MEM32(p + 28) = (uint32_t)(g_arena_end - g_arena_next);
    }
    STDRET(1);
}

/* ============================================================
 * Handles: host HANDLE is 64-bit, the simulated one is a 32-bit slot.
 * ============================================================ */

#define H_FILE_TAG   0xF11E0000u
#define H_FIND_TAG   0xF14D0000u
#define H_TAG_MASK   0xFFFF0000u
#define H_MAX        512

static HANDLE g_files[H_MAX];
static HANDLE g_finds[H_MAX];

static uint32_t put_handle(HANDLE* tbl, uint32_t tag, HANDLE h) {
    for (uint32_t i = 1; i < H_MAX; i++)
        if (!tbl[i]) { tbl[i] = h; return tag | i; }
    return 0xFFFFFFFFu;
}

static HANDLE get_handle(HANDLE* tbl, uint32_t tag, uint32_t v) {
    if ((v & H_TAG_MASK) != tag) return NULL;
    uint32_t i = v & 0xFFFFu;
    return i < H_MAX ? tbl[i] : NULL;
}

/* Std handles keep distinct sentinel values so GetFileType can answer for them
 * without a table entry. */
#define H_STDIN   0xF57D0000u
#define H_STDOUT  0xF57D0001u
#define H_STDERR  0xF57D0002u

void imp_GetStdHandle(void) {
    uint32_t which = ARG(0);
    RET(which == 0xFFFFFFF6u ? H_STDIN :
        which == 0xFFFFFFF5u ? H_STDOUT : H_STDERR);
    STDRET(1);
}

void imp_SetStdHandle(void)   { RET(1); STDRET(2); }
void imp_SetHandleCount(void) { RET(ARG(0)); STDRET(1); }

void imp_GetFileType(void) {
    uint32_t v = ARG(0);
    if ((v & H_TAG_MASK) == 0xF57D0000u) { RET(2); STDRET(1); return; } /* CHAR */
    HANDLE h = get_handle(g_files, H_FILE_TAG, v);
    RET(h ? FILE_TYPE_DISK : FILE_TYPE_UNKNOWN);
    STDRET(1);
}

void imp_CloseHandle(void) {
    uint32_t v = ARG(0);
    HANDLE h = get_handle(g_files, H_FILE_TAG, v);
    if (h) { CloseHandle(h); g_files[v & 0xFFFFu] = NULL; }
    RET(1);
    STDRET(1);
}

/* ============================================================
 * File I/O, forwarded to the host's own Win32.
 * ============================================================ */

void imp_CreateFileA(void) {
    const char* name = ARG(0) ? (const char*)ADDR(ARG(0)) : NULL;
    char redirected[MAX_PATH];
    name = redirect(name, redirected, sizeof redirected);
    HANDLE h = CreateFileA(name, ARG(1), ARG(2), NULL, ARG(4), ARG(5), NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_last_error = GetLastError();
        static int shown;         /* only the misses: they say what is sought */
        if (shown < 24) {
            fprintf(stderr, "[media] CreateFileA(\"%s\") MISS\n",
                    name ? name : "(null)");
            shown++;
        }
        RET(0xFFFFFFFFu);
    } else {
        RET(put_handle(g_files, H_FILE_TAG, h));
    }
    STDRET(7);
}

void imp_ReadFile(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    DWORD got = 0;
    BOOL ok = h ? ReadFile(h, (void*)ADDR(ARG(1)), ARG(2), &got, NULL) : FALSE;
    if (ARG(3)) MEM32(ARG(3)) = got;
    if (!ok) g_last_error = GetLastError();
    RET(ok ? 1 : 0);
    STDRET(5);
}

void imp_WriteFile(void) {
    uint32_t v = ARG(0);
    DWORD put = 0;
    BOOL ok;
    if ((v & H_TAG_MASK) == 0xF57D0000u) {      /* stdout / stderr */
        fwrite((const void*)ADDR(ARG(1)), 1, ARG(2),
               v == H_STDOUT ? stdout : stderr);
        put = ARG(2);
        ok = TRUE;
    } else {
        HANDLE h = get_handle(g_files, H_FILE_TAG, v);
        ok = h ? WriteFile(h, (const void*)ADDR(ARG(1)), ARG(2), &put, NULL)
               : FALSE;
    }
    if (ARG(3)) MEM32(ARG(3)) = put;
    if (!ok) g_last_error = GetLastError();
    RET(ok ? 1 : 0);
    STDRET(5);
}

void imp_SetFilePointer(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    LONG high = 0;
    LONG* phigh = ARG(2) ? &high : NULL;
    if (phigh) high = (LONG)MEM32(ARG(2));
    DWORD r = h ? SetFilePointer(h, (LONG)ARG(1), phigh, ARG(3))
                : INVALID_SET_FILE_POINTER;
    if (ARG(2)) MEM32(ARG(2)) = (uint32_t)high;
    RET(r);
    STDRET(4);
}

void imp_GetFileSize(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    DWORD high = 0;
    DWORD r = h ? GetFileSize(h, ARG(1) ? &high : NULL) : INVALID_FILE_SIZE;
    if (ARG(1)) MEM32(ARG(1)) = high;
    RET(r);
    STDRET(2);
}

void imp_FlushFileBuffers(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    RET(h && FlushFileBuffers(h) ? 1 : 0);
    STDRET(1);
}

void imp_SetEndOfFile(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    RET(h && SetEndOfFile(h) ? 1 : 0);
    STDRET(1);
}

void imp_GetFileAttributesA(void) {
    char buf[MAX_PATH];
    RET(ARG(0) ? GetFileAttributesA(
                     redirect((const char*)ADDR(ARG(0)), buf, sizeof buf))
               : INVALID_FILE_ATTRIBUTES);
    STDRET(1);
}

/* WIN32_FIND_DATAA has no pointers and identical layout in both word sizes, so
 * it can be copied straight across. */
void imp_FindFirstFileA(void) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((const char*)ADDR(ARG(0)), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        g_last_error = GetLastError();
        RET(0xFFFFFFFFu);
    } else {
        if (ARG(1)) memcpy((void*)ADDR(ARG(1)), &fd, sizeof fd);
        RET(put_handle(g_finds, H_FIND_TAG, h));
    }
    STDRET(2);
}

void imp_FindNextFileA(void) {
    HANDLE h = get_handle(g_finds, H_FIND_TAG, ARG(0));
    WIN32_FIND_DATAA fd;
    BOOL ok = h ? FindNextFileA(h, &fd) : FALSE;
    if (ok && ARG(1)) memcpy((void*)ADDR(ARG(1)), &fd, sizeof fd);
    RET(ok ? 1 : 0);
    STDRET(2);
}

void imp_FindClose(void) {
    uint32_t v = ARG(0);
    HANDLE h = get_handle(g_finds, H_FIND_TAG, v);
    if (h) { FindClose(h); g_finds[v & 0xFFFFu] = NULL; }
    RET(1);
    STDRET(1);
}

void imp_GetCurrentDirectoryA(void) {
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    RET(sim_copy_out(ARG(1), ARG(0), cwd));
    STDRET(2);
}

void imp_GetFullPathNameA(void) {
    char full[MAX_PATH];
    char* filepart = NULL;
    DWORD n = GetFullPathNameA((const char*)ADDR(ARG(0)), MAX_PATH, full,
                               &filepart);
    if (n && n < MAX_PATH) {
        uint32_t wrote = sim_copy_out(ARG(2), ARG(1), full);
        if (ARG(3) && filepart)
            MEM32(ARG(3)) = ARG(2) + (uint32_t)(filepart - full);
        RET(wrote);
    } else {
        RET(0);
    }
    STDRET(4);
}

/* ============================================================
 * Pointer probes. The CRT uses these before touching argv/environ, and a wrong
 * answer either crashes it or makes it discard good data.
 * ============================================================ */

static int readable(uint32_t va, uint32_t len) {
    if (!va || va < 0x10000u) return 0;
    if (va >= recomp_image_base() && va + len <= recomp_image_end()) return 1;
    if (va >= recomp_stack_base() && va + len <= recomp_stack_end()) return 1;
    if (va >= g_arena_base && va + len <= g_arena_committed) return 1;
    return 0;
}

void imp_IsBadReadPtr(void)  { RET(readable(ARG(0), ARG(1)) ? 0 : 1); STDRET(2); }
void imp_IsBadWritePtr(void) { RET(readable(ARG(0), ARG(1)) ? 0 : 1); STDRET(2); }
void imp_IsBadCodePtr(void)  { RET(readable(ARG(0), 1) ? 0 : 1); STDRET(1); }

/* ============================================================
 * Critical sections. Single-threaded here, so these are bookkeeping only --
 * but the CRT reads back what it wrote, so the struct must be left alone.
 * ============================================================ */

void imp_InitializeCriticalSection(void) { STDRET(1); }
void imp_DeleteCriticalSection(void)     { STDRET(1); }
void imp_EnterCriticalSection(void)      { STDRET(1); }
void imp_LeaveCriticalSection(void)      { STDRET(1); }

void imp_InterlockedExchange(void) {
    uint32_t p = ARG(0), v = ARG(1), old = 0;
    if (p) { old = MEM32(p); MEM32(p) = v; }
    RET(old);
    STDRET(2);
}

/* ============================================================
 * Locale. The CRT runs this before main and checks the results.
 * ============================================================ */

void imp_GetACP(void)   { RET(1252); STDRET(0); }
void imp_GetOEMCP(void) { RET(437);  STDRET(0); }

void imp_GetCPInfo(void) {
    uint32_t p = ARG(1);
    if (p) {
        CPINFO ci;
        if (!GetCPInfo(ARG(0), &ci)) { RET(0); STDRET(2); return; }
        memcpy((void*)ADDR(p), &ci, sizeof ci);   /* no pointers inside */
    }
    RET(1);
    STDRET(2);
}

void imp_GetStringTypeA(void) {
    RET(GetStringTypeA(ARG(0), ARG(1), (LPCSTR)ADDR(ARG(2)), (int)ARG(3),
                       ARG(4) ? (LPWORD)ADDR(ARG(4)) : NULL) ? 1 : 0);
    STDRET(5);
}

void imp_GetStringTypeW(void) {
    RET(GetStringTypeW(ARG(0), (LPCWSTR)ADDR(ARG(1)), (int)ARG(2),
                       ARG(3) ? (LPWORD)ADDR(ARG(3)) : NULL) ? 1 : 0);
    STDRET(4);
}

void imp_LCMapStringA(void) {
    RET(LCMapStringA(ARG(0), ARG(1), (LPCSTR)ADDR(ARG(2)), (int)ARG(3),
                     ARG(4) ? (LPSTR)ADDR(ARG(4)) : NULL, (int)ARG(5)));
    STDRET(6);
}

void imp_LCMapStringW(void) {
    RET(LCMapStringW(ARG(0), ARG(1), (LPCWSTR)ADDR(ARG(2)), (int)ARG(3),
                     ARG(4) ? (LPWSTR)ADDR(ARG(4)) : NULL, (int)ARG(5)));
    STDRET(6);
}

void imp_MultiByteToWideChar(void) {
    RET(MultiByteToWideChar(ARG(0), ARG(1), (LPCSTR)ADDR(ARG(2)), (int)ARG(3),
                            ARG(4) ? (LPWSTR)ADDR(ARG(4)) : NULL, (int)ARG(5)));
    STDRET(6);
}

void imp_WideCharToMultiByte(void) {
    RET(WideCharToMultiByte(ARG(0), ARG(1), (LPCWSTR)ADDR(ARG(2)), (int)ARG(3),
                            ARG(4) ? (LPSTR)ADDR(ARG(4)) : NULL, (int)ARG(5),
                            ARG(6) ? (LPCSTR)ADDR(ARG(6)) : NULL,
                            ARG(7) ? (LPBOOL)ADDR(ARG(7)) : NULL));
    STDRET(8);
}

void imp_CompareStringA(void) {
    RET(CompareStringA(ARG(0), ARG(1), (LPCSTR)ADDR(ARG(2)), (int)ARG(3),
                       (LPCSTR)ADDR(ARG(4)), (int)ARG(5)));
    STDRET(6);
}

void imp_CompareStringW(void) {
    RET(CompareStringW(ARG(0), ARG(1), (LPCWSTR)ADDR(ARG(2)), (int)ARG(3),
                       (LPCWSTR)ADDR(ARG(4)), (int)ARG(5)));
    STDRET(6);
}

/* ============================================================
 * Time
 * ============================================================ */

void imp_GetTickCount(void)  { RET(GetTickCount()); STDRET(0); }
void imp_timeGetTime(void)   { RET(GetTickCount()); STDRET(0); }
void imp_timeBeginPeriod(void) { RET(0); STDRET(1); }
void imp_timeEndPeriod(void)   { RET(0); STDRET(1); }
void imp_Sleep(void)         { Sleep(ARG(0)); STDRET(1); }

void imp_QueryPerformanceCounter(void) {
    LARGE_INTEGER li; QueryPerformanceCounter(&li);
    if (ARG(0)) { MEM32(ARG(0)) = li.LowPart; MEM32(ARG(0) + 4) = (uint32_t)li.HighPart; }
    RET(1);
    STDRET(1);
}

void imp_QueryPerformanceFrequency(void) {
    LARGE_INTEGER li; QueryPerformanceFrequency(&li);
    if (ARG(0)) { MEM32(ARG(0)) = li.LowPart; MEM32(ARG(0) + 4) = (uint32_t)li.HighPart; }
    RET(1);
    STDRET(1);
}

/* SYSTEMTIME and FILETIME are all fixed-width fields -- no marshalling. */
void imp_GetLocalTime(void) {
    if (ARG(0)) GetLocalTime((LPSYSTEMTIME)ADDR(ARG(0)));
    STDRET(1);
}

void imp_GetSystemTime(void) {
    if (ARG(0)) GetSystemTime((LPSYSTEMTIME)ADDR(ARG(0)));
    STDRET(1);
}

void imp_GetSystemTimeAsFileTime(void) {
    if (ARG(0)) GetSystemTimeAsFileTime((LPFILETIME)ADDR(ARG(0)));
    STDRET(1);
}

/* ============================================================
 * Devices with no implementation yet. These report ABSENT rather than success:
 * MMSYSERR_NOERROR, DD_OK and DS_OK are all zero, so a stub returning 0 would
 * claim the device exists and leave the caller's out pointer uninitialised.
 * ============================================================ */

void imp_waveOutGetNumDevs(void) { RET(0); STDRET(0); }   /* 0 devices: honest */

void imp_DirectDrawCreate(void) {
    IMPORT_STUB("DirectDrawCreate");
    if (ARG(1)) MEM32(ARG(1)) = 0;        /* never leave the out param stale */
    RET(0x8007000Eu);                     /* DDERR_OUTOFMEMORY: a real failure */
    STDRET(3);
}

/* ============================================================
 * APIs whose whole job is to fill a caller-provided struct.
 *
 * These are the ones a stub hurts most. GetTimeZoneInformation returning 0 is
 * TIME_ZONE_ID_UNKNOWN -- a perfectly valid answer -- so the CRT believed it
 * and then sprintf'd a %s straight out of the struct it had not written,
 * faulting on a garbage pointer two frames away.
 * ============================================================ */

/* TIME_ZONE_INFORMATION is fixed-width throughout (LONG, WCHAR[32], SYSTEMTIME)
 * with no pointers, so it is identical in both word sizes. */
void imp_GetTimeZoneInformation(void) {
    TIME_ZONE_INFORMATION tz;
    DWORD rc = GetTimeZoneInformation(&tz);
    if (ARG(0)) memcpy((void*)ADDR(ARG(0)), &tz, sizeof tz);
    RET(rc);
    STDRET(1);
}

/* SYSTEM_INFO does contain pointers, so the 32-bit layout is built field by
 * field. The address-space limits are the simulated ones, not the host's. */
void imp_GetSystemInfo(void) {
    uint32_t p = ARG(0);
    if (p) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        MEM32(p +  0) = 0;                       /* dwOemId */
        MEM32(p +  4) = si.dwPageSize;
        MEM32(p +  8) = 0x00010000u;             /* lpMinimumApplicationAddress */
        MEM32(p + 12) = 0x7FFEFFFFu;             /* lpMaximumApplicationAddress */
        MEM32(p + 16) = 1;                       /* dwActiveProcessorMask */
        MEM32(p + 20) = 1;                       /* dwNumberOfProcessors */
        MEM32(p + 24) = 586;                     /* dwProcessorType: PROCESSOR_INTEL_PENTIUM */
        MEM32(p + 28) = si.dwAllocationGranularity;
        MEM16(p + 32) = 5;                       /* wProcessorLevel */
        MEM16(p + 34) = 0;                       /* wProcessorRevision */
    }
    STDRET(1);
}

void imp_GetUserNameA(void) {
    char name[256] = "player";
    DWORD n = sizeof name;
    if (!GetUserNameA(name, &n)) snprintf(name, sizeof name, "player");
    uint32_t cap = ARG(1) ? MEM32(ARG(1)) : 0;
    uint32_t wrote = sim_copy_out(ARG(0), cap, name);
    if (ARG(1)) MEM32(ARG(1)) = wrote + 1;       /* includes the terminator */
    RET(1);
    STDRET(2);
}

void imp_GetWindowsDirectoryA(void) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    RET(sim_copy_out(ARG(0), ARG(1), dir));
    STDRET(2);
}

void imp_GetSystemDirectoryA(void) {
    char dir[MAX_PATH];
    GetSystemDirectoryA(dir, MAX_PATH);
    RET(sim_copy_out(ARG(0), ARG(1), dir));
    STDRET(2);
}

void imp_GetTempPathA(void) {
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    RET(sim_copy_out(ARG(1), ARG(0), dir));
    STDRET(2);
}

/*
 * The engine walks A: to Z: looking for a drive that reports DRIVE_CDROM, then
 * looks for its data there. On a machine with no optical drive -- which is most
 * of them now -- it finds none and stops at "Please insert the MechCommander
 * Expansion compact disc".
 *
 * The data is not on a disc here; it is in the install directory, which is
 * where the game is being run from. So the drive holding the install answers
 * "that is the media" and every other drive answers truthfully. This is a media
 * PATH question, not a protection one: MCX.EXE has no DRM of any kind (no
 * packer, no protection indicators -- see the Phase 0 notes), and the check is
 * simply the 1999 way of asking where the content lives.
 */
void imp_GetDriveTypeA(void) {
    const char* q = ARG(0) ? (const char*)ADDR(ARG(0)) : NULL;
    UINT t = q ? GetDriveTypeA(q) : DRIVE_UNKNOWN;
    if (q && g_program[0] &&
        (q[0] | 32) == (g_program[0] | 32) && q[1] == ':')
        t = DRIVE_CDROM;
    RET(t);
    STDRET(1);
}

/* ============================================================
 * The registry, forwarded to the host's. MechCommander keeps its settings in
 * HKCU like any other Win32 app, so there is nothing to emulate -- only the
 * handle to translate. The predefined roots (HKEY_CURRENT_USER and friends)
 * are the same 32-bit constants on both sides, so they pass straight through.
 * ============================================================ */

#define H_KEY_TAG  0xF4E70000u
static HKEY g_keys[H_MAX];

static HKEY map_hkey(uint32_t v) {
    if (v >= 0x80000000u) return (HKEY)(ULONG_PTR)v;       /* predefined root */
    if ((v & H_TAG_MASK) != H_KEY_TAG) return NULL;
    uint32_t i = v & 0xFFFFu;
    return i < H_MAX ? g_keys[i] : NULL;
}

static uint32_t put_hkey(HKEY h) {
    for (uint32_t i = 1; i < H_MAX; i++)
        if (!g_keys[i]) { g_keys[i] = h; return H_KEY_TAG | i; }
    return 0;
}

/* An out handle that is left stale is the whole reason these are not stubs:
 * ERROR_SUCCESS is zero, so a stub "succeeds" and the caller then uses
 * whatever was already in its HKEY variable. */
static void reg_open(uint32_t root, uint32_t subkey, uint32_t out, LONG rc_arg) {
    (void)rc_arg;
    HKEY parent = map_hkey(root), opened = NULL;
    LONG rc = parent ? RegOpenKeyExA(parent,
                                     subkey ? (LPCSTR)ADDR(subkey) : NULL,
                                     0, KEY_READ | KEY_WRITE, &opened)
                     : ERROR_INVALID_HANDLE;
    if (rc != ERROR_SUCCESS && parent)      /* fall back to read-only */
        rc = RegOpenKeyExA(parent, subkey ? (LPCSTR)ADDR(subkey) : NULL,
                           0, KEY_READ, &opened);
    if (out) MEM32(out) = (rc == ERROR_SUCCESS) ? put_hkey(opened) : 0;
    RET(rc);
}

void imp_RegOpenKeyA(void)   { reg_open(ARG(0), ARG(1), ARG(2), 0); STDRET(3); }
void imp_RegOpenKeyExA(void) { reg_open(ARG(0), ARG(1), ARG(4), 0); STDRET(5); }

void imp_RegCloseKey(void) {
    uint32_t v = ARG(0);
    HKEY h = map_hkey(v);
    if (h && v < 0x80000000u) { RegCloseKey(h); g_keys[v & 0xFFFFu] = NULL; }
    RET(ERROR_SUCCESS);
    STDRET(1);
}

void imp_RegQueryValueExA(void) {
    HKEY h = map_hkey(ARG(0));
    DWORD type = 0, cb = ARG(5) ? MEM32(ARG(5)) : 0;
    LONG rc = h ? RegQueryValueExA(h, ARG(1) ? (LPCSTR)ADDR(ARG(1)) : NULL, NULL,
                                   &type, ARG(4) ? (LPBYTE)ADDR(ARG(4)) : NULL,
                                   ARG(5) ? &cb : NULL)
                : ERROR_INVALID_HANDLE;
    if (ARG(3)) MEM32(ARG(3)) = type;
    if (ARG(5)) MEM32(ARG(5)) = cb;
    RET(rc);
    STDRET(6);
}

void imp_RegSetValueExA(void) {
    HKEY h = map_hkey(ARG(0));
    LONG rc = h ? RegSetValueExA(h, ARG(1) ? (LPCSTR)ADDR(ARG(1)) : NULL, 0,
                                 ARG(3), ARG(4) ? (const BYTE*)ADDR(ARG(4)) : NULL,
                                 ARG(5))
                : ERROR_INVALID_HANDLE;
    RET(rc);
    STDRET(6);
}

void imp_RegFlushKey(void) {
    HKEY h = map_hkey(ARG(0));
    RET(h ? RegFlushKey(h) : ERROR_INVALID_HANDLE);
    STDRET(1);
}

/* ============================================================
 * Modules. A real 32-bit DLL cannot be loaded into the simulated address space
 * -- its code would be host code, not lifted code -- so this reports failure
 * rather than handing back a handle nothing can service. The name is logged,
 * because what the game wanted to load is worth knowing.
 * ============================================================ */

/*
 * Modules loaded at runtime.
 *
 * Nothing is actually mapped -- a real 32-bit DLL's code would be host code,
 * not lifted code. What the handle buys is a name to resolve exports against,
 * and the exports that matter have shims with derived argument counts
 * (mcg_dyn_exports). MechCommander loads imagehlp.dll for its crash reporter
 * and calls a failure fatal, so it never reaches its main loop without this.
 */
#define H_MODULE_TAG  0xD11D0000u
#define MODULE_MAX    32

static char g_modules[MODULE_MAX][64];
static uint32_t g_next_module = 1;

/* ".\\IMAGEHLP.DLL" and "imagehlp.dll" are the same module. */
static const char* base_name(const char* p) {
    const char* s = p;
    for (const char* q = p; *q; q++)
        if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}

void imp_LoadLibraryA(void) {
    const char* name = ARG(0) ? (const char*)ADDR(ARG(0)) : "";
    uint32_t i = g_next_module;
    if (i >= MODULE_MAX) { g_last_error = ERROR_TOO_MANY_MODULES; RET(0); STDRET(1); return; }
    g_next_module++;
    snprintf(g_modules[i], sizeof g_modules[i], "%s", base_name(name));
    fprintf(stderr, "[shims] LoadLibraryA(\"%s\") -> %08X (exports shimmed)\n",
            name, H_MODULE_TAG | i);
    RET(H_MODULE_TAG | i);
    STDRET(1);
}

void imp_FreeLibrary(void)    { RET(1); STDRET(1); }
void imp_GetProcAddress(void) {
    uint32_t h = ARG(0), nm = ARG(1);
    uint32_t va = 0;
    /* An export requested by ordinal arrives as a small integer, not a
     * pointer. We only implement exports by name. */
    if (nm > 0xFFFF && (h & H_TAG_MASK) == H_MODULE_TAG) {
        uint32_t i = h & 0xFFFFu;
        if (i < MODULE_MAX)
            va = mcg_dyn_resolve(g_modules[i], (const char*)ADDR(nm));
    }
    if (!va) {
        fprintf(stderr, "[shims] GetProcAddress(%08X, %s) -> not implemented\n",
                h, nm > 0xFFFF ? (const char*)ADDR(nm) : "(ordinal)");
        g_last_error = ERROR_PROC_NOT_FOUND;
    }
    RET(va);
    STDRET(2);
}

/* ============================================================
 * Dialogs. During bring-up a message box is usually the game explaining what
 * it could not find, so the text goes to the log rather than to a window.
 * ============================================================ */

void imp_MessageBoxA(void) {
    static int shown;
    if (shown < 3) {
        fprintf(stderr, "[MessageBox] from 0x%08X: %s | %s\n", g_cur_func,
                ARG(2) ? (const char*)ADDR(ARG(2)) : "(no caption)",
                ARG(1) ? (const char*)ADDR(ARG(1)) : "(no text)");
        shown++;
    }
    RET(1);                               /* IDOK */
    STDRET(4);
}

/* ============================================================
 * Exit really exits. A stub that returns lets the game run past its own
 * decision to quit, and everything after that is code the original would never
 * have executed.
 * ============================================================ */

void imp_ExitProcess(void) {
    fprintf(stderr, "[runtime] ExitProcess(%u) after %u indirect calls\n",
            ARG(0), g_icall_count);
    fflush(NULL);
    ExitProcess(ARG(0));
}

void imp_TerminateProcess(void) {
    fprintf(stderr, "[runtime] TerminateProcess(%u)\n", ARG(1));
    fflush(NULL);
    ExitProcess(ARG(1));
}

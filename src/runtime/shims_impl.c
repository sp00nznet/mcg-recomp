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
uint32_t shims_arena_committed(void) { return g_arena_committed; }

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

/* Simulated memory for the other runtime pieces (the DirectDraw mock builds
 * its COM objects and vtables out of this). */
uint32_t shims_alloc(uint32_t n) { return arena_alloc(n); }

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

/*
 * MCG_IOLOG=1 narrates every seek and read, by file.
 *
 * Content the engine loads from a .FST never reaches CreateFileA under its own
 * name -- the archive is opened once and everything after that is a seek and a
 * read inside it. So "did it find campaign.fit" is not answerable from the
 * misses; it is answerable from whether anything ever seeks to that packet's
 * offset and reads its bytes.
 */
static char g_fnames[H_MAX][32];
static int  g_iolog = -1;

static int iolog_on(void) {
    if (g_iolog < 0) { const char* e = getenv("MCG_IOLOG"); g_iolog = e && *e != '0'; }
    return g_iolog;
}

static const char* fname_of(uint32_t v) {
    uint32_t i = v & 0xFFFFu;
    return i < H_MAX && g_fnames[i][0] ? g_fnames[i] : "?";
}

void imp_CreateFileA(void) {
    const char* name = ARG(0) ? (const char*)ADDR(ARG(0)) : NULL;
    char redirected[MAX_PATH];
    name = redirect(name, redirected, sizeof redirected);
    HANDLE h = CreateFileA(name, ARG(1), ARG(2), NULL, ARG(4), ARG(5), NULL);
    if (h == INVALID_HANDLE_VALUE) {
        g_last_error = GetLastError();
        /* Only the misses: they say what the engine is looking for. The cap
         * used to be 24, which is exactly how many font files it probes before
         * falling back to the packfile -- so every later miss was swallowed. */
        static int shown;
        if (shown < 60) {
            fprintf(stderr, "[media] CreateFileA(\"%s\") MISS\n",
                    name ? name : "(null)");
            shown++;
        }
        RET(0xFFFFFFFFu);
    } else {
        uint32_t v = put_handle(g_files, H_FILE_TAG, h);
        uint32_t i = v & 0xFFFFu;
        if (i < H_MAX && name) {
            const char* base = name;
            for (const char* p = name; *p; p++)
                if (*p == '\\' || *p == '/') base = p + 1;
            snprintf(g_fnames[i], sizeof g_fnames[i], "%s", base);
        }
        if (iolog_on()) fprintf(stderr, "[io] open %s\n", name ? name : "?");
        RET(v);
    }
    STDRET(7);
}

void imp_ReadFile(void) {
    HANDLE h = get_handle(g_files, H_FILE_TAG, ARG(0));
    DWORD got = 0;
    BOOL ok = h ? ReadFile(h, (void*)ADDR(ARG(1)), ARG(2), &got, NULL) : FALSE;
    if (ARG(3)) MEM32(ARG(3)) = got;
    if (!ok) g_last_error = GetLastError();
    if (iolog_on())
        fprintf(stderr, "[io] read %s %u -> %lu%s\n", fname_of(ARG(0)),
                ARG(2), got, ok ? "" : " FAILED");
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
    if (iolog_on())
        fprintf(stderr, "[io] seek %s %+d from %u -> %lu\n", fname_of(ARG(0)),
                (int)ARG(1), ARG(3), r);
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
void imp_Sleep(void) {
    uint32_t ms = ARG(0);
    /* The frame loop computes its sleep through __ftol, so a bad x87 result
     * turns "wait a few milliseconds" into a stall. Worth seeing. */
    { static int shown; if (shown < 6) { shown++;
        fprintf(stderr, "[time] Sleep(%u)\n", ms); } }
    Sleep(ms);
    STDRET(1);
}

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

/* One device. This was 0 while there was genuinely nothing behind it, but the
 * DirectSound mock is a real (silent) output now, and the engine treats zero
 * wave devices as a hard failure -- "No Digital Sound Hardware Installed" --
 * which leaves its sound objects unconstructed and the logistics UI drawing
 * through a vtable pointer that was never written. */
void imp_waveOutGetNumDevs(void) { RET(1); STDRET(0); }

/* ============================================================
 * COM creation
 *
 * SessionManager::CreateDirectPlayInterface does not call
 * DirectPlayLobbyCreateA at all -- it asks COM for
 * CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay3A) and stores the result
 * at this+0x13C. S_OK is zero, so the stub "succeeded" while leaving that
 * member null, and the engine's own Assert said exactly that one instruction
 * before dereferencing it.
 *
 * REGDB_E_CLASSNOTREG is a path the engine already handles (it tests for that
 * specific code), so an unknown class gets the honest answer rather than a
 * plausible one.
 * ============================================================ */

static const uint8_t CLSID_DIRECTPLAY[16] = {
    0x20, 0x6D, 0xEB, 0xD1, 0x23, 0x89, 0xD0, 0x11,
    0x9D, 0x97, 0x00, 0xA0, 0xC9, 0x0A, 0x43, 0xCB,
};

void imp_CoInitialize(void)   { RET(0); STDRET(1); }
void imp_CoUninitialize(void) { STDRET(0); }

void imp_CoCreateInstance(void) {
    uint32_t clsid = ARG(0), ppv = ARG(4);
    if (ppv) MEM32(ppv) = 0;          /* never leave the out param stale */
    if (clsid && !memcmp((const void*)ADDR(clsid), CLSID_DIRECTPLAY, 16)) {
        uint32_t obj = dplay_create();
        if (ppv) MEM32(ppv) = obj;
        RET(obj ? 0u : 0x8007000Eu);
    } else {
        fprintf(stderr, "[com] CoCreateInstance {%08X...} not registered\n",
                clsid ? MEM32(clsid) : 0);
        RET(0x80040154u);             /* REGDB_E_CLASSNOTREG */
    }
    STDRET(5);
}

/* DPLAYX ordinal 4. Five arguments, per the decoration on the game's own
 * import thunk (_DirectPlayLobbyCreateA@20). */
void imp_DirectPlayLobbyCreateA(void) {
    uint32_t obj = dplay_lobby_create();
    if (ARG(1)) MEM32(ARG(1)) = obj;
    RET(obj ? 0u : 0x80004005u);
    STDRET(5);
}

/* DSOUND is imported by ordinal; the resolver names it DirectSoundCreate. */
void imp_DirectSoundCreate(void) {
    uint32_t obj = dsound_create();
    if (ARG(1)) MEM32(ARG(1)) = obj;
    RET(obj ? 0u : 0x8007000Eu);
    STDRET(3);
}

void imp_DirectDrawCreate(void) {
    uint32_t obj = ddraw_create();
    if (ARG(1)) MEM32(ARG(1)) = obj;      /* never leave the out param stale */
    RET(obj ? 0u : 0x8007000Eu);          /* DD_OK, else DDERR_OUTOFMEMORY */
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
 * Host objects behind 32-bit handles
 *
 * HWND, HDC and the GDI objects are all host pointers, which are 64 bits here
 * and a 32-bit slot over there. They live in one table, and h_put is
 * deduplicating: the game compares handles for equality, so the same host
 * pointer has to come back as the same 32-bit value every time.
 *
 * The range sits well above every simulated region (image, stack, arena and the
 * runtime-export VAs all end below 0x11000000), so a handle used as a pointer
 * by mistake faults instead of landing in real memory.
 * ============================================================ */

#define H_OBJ_BASE  0x40000000u
#define H_OBJ_MAX   4096

static void* g_objs[H_OBJ_MAX];
static uint32_t g_obj_n = 1;

static uint32_t h_put(void* p) {
    if (!p) return 0;
    for (uint32_t i = 1; i < g_obj_n; i++)
        if (g_objs[i] == p) return H_OBJ_BASE + i * 4;
    if (g_obj_n >= H_OBJ_MAX) return 0;
    g_objs[g_obj_n] = p;
    return H_OBJ_BASE + (g_obj_n++) * 4;
}

static void* h_ptr(uint32_t v) {
    if (v < H_OBJ_BASE) return NULL;
    uint32_t i = (v - H_OBJ_BASE) / 4;
    return (i && i < g_obj_n) ? g_objs[i] : NULL;
}

/* For the other runtime pieces: the DirectDraw mock needs the real HWND that
 * SetCooperativeLevel is handed. */
void* shims_handle(uint32_t v) { return h_ptr(v); }

#define HWND_OF(v)  ((HWND)h_ptr(v))
#define HDC_OF(v)   ((HDC)h_ptr(v))

/* A simulated pointer used directly as a host one. Simulated memory IS host
 * memory at a fixed base, so strings and structs need no copy -- only null has
 * to survive the trip. */
static void* simp(uint32_t va) { return va ? (void*)ADDR(va) : NULL; }

/* ============================================================
 * Calling back INTO lifted code
 *
 * A window procedure is the one place the calling convention is dictated by
 * Windows rather than by the game's compiler, so restore esp afterwards instead
 * of trusting the callee's epilogue -- that way it works for a stdcall or a
 * cdecl body. Re-entrancy is expected: this runs while lifted code is already
 * inside its own message pump, and the new frame just sits below it.
 * ============================================================ */

static uint32_t call_lifted(uint32_t va, int argc, const uint32_t* argv) {
    recomp_func_t f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup(va);
    if (!f) {
        fprintf(stderr, "[shims] callback into unlifted VA 0x%08X\n", va);
        return 0;
    }
    /*
     * Save and restore the WHOLE register file, not just esp.
     *
     * On real hardware a window procedure or a timer callback arrives on its
     * own thread with its own registers, and the interrupted code never
     * notices. Here there is one global register file, so running a callback
     * inline hands the interrupted function's eax/ecx/edx/ebx/esi/edi/ebp to
     * whatever the callback does with them. aSystem::run keeps its loop state
     * in esi and edi across the PeekMessage call; MouseTimer, fired from inside
     * that shim, walked off with them and the frame loop never came back.
     *
     * The x87 stack goes too -- it is equally global, and a callback that does
     * any float arithmetic would otherwise leave the interrupted code's
     * operands rearranged underneath it.
     */
    uint32_t s_eax = g_eax, s_ecx = g_ecx, s_edx = g_edx, s_ebx = g_ebx;
    uint32_t s_esp = g_esp, s_ebp = g_ebp, s_esi = g_esi, s_edi = g_edi;
    int s_fp_top = g_fp_top;
    uint16_t s_fpu_cw = g_fpu_cw;
    double s_st[8];
    memcpy(s_st, g_st, sizeof s_st);

    for (int i = argc - 1; i >= 0; i--)
        PUSH32(g_esp, argv[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    uint32_t ret = g_eax;

    g_eax = s_eax; g_ecx = s_ecx; g_edx = s_edx; g_ebx = s_ebx;
    g_esp = s_esp; g_ebp = s_ebp; g_esi = s_esi; g_edi = s_edi;
    g_fp_top = s_fp_top;
    g_fpu_cw = s_fpu_cw;
    memcpy(g_st, s_st, sizeof g_st);
    return ret;
}

/* ============================================================
 * Window classes, windows and the message pump
 * ============================================================ */

/* WNDCLASSA, 32-bit: 40 bytes, all fields pointer-or-int wide. */
#define WC_STYLE 0x00
#define WC_PROC  0x04
#define WC_CLSX  0x08
#define WC_WNDX  0x0C
#define WC_INST  0x10
#define WC_ICON  0x14
#define WC_CURS  0x18
#define WC_BRUSH 0x1C
#define WC_MENU  0x20
#define WC_NAME  0x24

#define MAX_CLASSES 32
static struct { ATOM atom; uint32_t proc; } g_classes[MAX_CLASSES];
static uint32_t g_class_n;

static uint32_t class_proc(ATOM a) {
    for (uint32_t i = 0; i < g_class_n; i++)
        if (g_classes[i].atom == a) return g_classes[i].proc;
    return 0;
}

/* A pointer lParam cannot survive the 32-bit round trip: WM_NCCREATE carries a
 * host CREATESTRUCT*, and the game hands it straight back to DefWindowProcA,
 * which would dereference a truncated half. So remember the real lParam for the
 * message in flight and give it back on the way out.
 *
 * ponytail: pass-through only. If the engine ever *reads* a pointer lParam it
 * faults on the truncated address rather than reading something plausible,
 * which is the failure worth having -- marshal that message's struct then. */
typedef struct { HWND hwnd; UINT msg; LPARAM lp; } cur_msg_t;
/* Non-zero while lifted code is running inside a window procedure --
 * printed on a fault, because a crash reached that way means the engine
 * was re-entered while it was still building whatever it drew. */
int g_wndproc_depth;
static cur_msg_t g_cur_msg;

static LRESULT CALLBACK host_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    uint32_t proc = class_proc((ATOM)GetClassLongPtrA(hwnd, GCW_ATOM));
    if (!proc) return DefWindowProcA(hwnd, msg, wp, lp);
    g_wndproc_depth++;
    g_n_wndproc++;
    /*
     * Keep the engine active.
     *
     * aSystem::run blocks in WaitMessage whenever the engine thinks it has lost
     * the display -- correct for a 1999 fullscreen-exclusive title, which
     * should yield the CPU when it is not on screen. Here it never gets the
     * screen in the first place: the game runs in a window, and whatever
     * launched it keeps the foreground, so the engine is told active=0 during
     * startup and parks before drawing its first frame.
     *
     * So WM_ACTIVATEAPP is always delivered as activating. This is a windowed
     * port's decision, the same one source ports make when they add a "run in
     * background" option, not a workaround for a lifting bug -- the engine's
     * own logic is working exactly as written.
     */
    if (msg == WM_ACTIVATEAPP) {
        fprintf(stderr, "[win] WM_ACTIVATEAPP active=%u -> forced active\n",
                (unsigned)wp);
        wp = TRUE;
    }
    { static int shown; if (shown < 24) { shown++;
        fprintf(stderr, "[win] wndproc msg=%04X (depth %d)\n", msg,
                g_wndproc_depth); } }
    cur_msg_t outer = g_cur_msg;            /* nested dispatches nest correctly */
    g_cur_msg.hwnd = hwnd;
    g_cur_msg.msg = msg;
    g_cur_msg.lp = lp;
    /* h_put, not h_ptr: WM_NCCREATE arrives before CreateWindowExA has
     * returned, so this is where the window first gets a 32-bit identity. */
    uint32_t args[4] = { h_put(hwnd), msg, (uint32_t)wp, (uint32_t)lp };
    uint32_t r = call_lifted(proc, 4, args);
    /*
     * WM_PAINT must leave the window validated, or Windows regenerates it
     * immediately and the queue never empties.
     *
     * A fullscreen DirectDraw title has no reason to handle WM_PAINT -- it
     * owns the screen and redraws from its own loop -- so the engine returns
     * without calling BeginPaint/EndPaint. Windowed, that is an endless paint
     * storm: PeekMessage returned a message every single time (peek and
     * dispatch climbing together at ~1700/s) and aSystem::run never got past
     * the pump to UpdateDisplay. Hence a window that was running hard and
     * drawing nothing.
     */
    if (msg == WM_PAINT)
        ValidateRect(hwnd, NULL);
    g_cur_msg = outer;
    g_wndproc_depth--;
    return (LRESULT)(LONG_PTR)(int32_t)r;
}

void imp_RegisterClassA(void) {
    uint32_t wc = ARG(0);
    if (!wc || g_class_n >= MAX_CLASSES) { RET(0); STDRET(1); return; }
    WNDCLASSA c;
    memset(&c, 0, sizeof c);
    c.style         = MEM32(wc + WC_STYLE);
    c.lpfnWndProc   = host_wndproc;
    c.cbClsExtra    = (int)MEM32(wc + WC_CLSX);
    c.cbWndExtra    = (int)MEM32(wc + WC_WNDX);
    c.hInstance     = GetModuleHandleA(NULL);
    c.hIcon         = (HICON)h_ptr(MEM32(wc + WC_ICON));
    c.hCursor       = (HCURSOR)h_ptr(MEM32(wc + WC_CURS));
    c.hbrBackground = (HBRUSH)h_ptr(MEM32(wc + WC_BRUSH));
    c.lpszMenuName  = (LPCSTR)simp(MEM32(wc + WC_MENU));
    c.lpszClassName = (LPCSTR)simp(MEM32(wc + WC_NAME));
    if (!c.hCursor) c.hCursor = LoadCursorA(NULL, IDC_ARROW);
    ATOM a = RegisterClassA(&c);
    if (!a) {
        g_last_error = GetLastError();
        fprintf(stderr, "[win] RegisterClassA failed (%u)\n", g_last_error);
        RET(0);
        STDRET(1);
        return;
    }
    g_classes[g_class_n].atom = a;
    g_classes[g_class_n].proc = MEM32(wc + WC_PROC);
    fprintf(stderr, "[win] RegisterClassA(\"%s\") wndproc=0x%08X\n",
            c.lpszClassName ? c.lpszClassName : "?", g_classes[g_class_n].proc);
    g_class_n++;
    RET(a);
    STDRET(1);
}

void imp_CreateWindowExA(void) {
    /* hMenu and lpParam pass as NULL: a menu would be a simulated handle with
     * no host object behind it, and lpParam only reaches the game again inside
     * a CREATESTRUCT full of host pointers, which is worse than not offering
     * it. ponytail: marshal a CREATESTRUCT if one is ever actually read. */
    DWORD style = ARG(3), ex = ARG(0);
    int x = (int)ARG(4), y = (int)ARG(5), w = (int)ARG(6), h2 = (int)ARG(7);

    /*
     * Give the game a real window.
     *
     * It asks for a borderless WS_POPUP at 0,0 the size of the display mode,
     * which is exactly right for a fullscreen-exclusive title and exactly
     * wrong here: it covers the desktop with something that cannot be moved,
     * resized or closed. A framed window with the same CLIENT size is the same
     * picture with a way out of it.
     *
     * If the requested size does not fit the work area the frame is shrunk to
     * fit; present() already StretchBlts when the client and the surface
     * differ, so the engine still renders at its own resolution.
     */
    if ((style & WS_POPUP) && !(style & WS_CHILD)) {
        RECT wa = { 0, 0, 0, 0 };
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        int maxw = (wa.right - wa.left) - 80, maxh = (wa.bottom - wa.top) - 120;
        if (maxw > 0 && maxh > 0 && w > 0 && h2 > 0) {
            double sc = 1.0;
            if (w > maxw) sc = (double)maxw / w;
            if (h2 * sc > maxh) sc = (double)maxh / h2;
            w = (int)(w * sc);
            h2 = (int)(h2 * sc);
        }
        style = (style & ~(DWORD)WS_POPUP) | WS_OVERLAPPEDWINDOW;
        ex &= ~(DWORD)WS_EX_TOPMOST;
        RECT r = { 0, 0, w, h2 };
        AdjustWindowRect(&r, style, FALSE);
        x = wa.left + 40;
        y = wa.top + 40;
        w = r.right - r.left;
        h2 = r.bottom - r.top;
        fprintf(stderr, "[win] popup -> framed window, client %dx%d\n",
                (int)ARG(6), (int)ARG(7));
    }

    HWND h = CreateWindowExA(ex, (LPCSTR)simp(ARG(1)), (LPCSTR)simp(ARG(2)),
                             style, x, y, w, h2,
                             HWND_OF(ARG(8)), NULL, GetModuleHandleA(NULL), NULL);
    fprintf(stderr, "[win] CreateWindowExA(\"%s\" style=%08X %dx%d) -> %p\n",
            (const char*)simp(ARG(1)), style, w, h2, (void*)h);
    if (!h) {
        g_last_error = GetLastError();
        fprintf(stderr, "[win] CreateWindowExA failed (%u)\n", g_last_error);
    }
    RET(h ? h_put(h) : 0);
    STDRET(12);
}

void imp_DefWindowProcA(void) {
    HWND h = HWND_OF(ARG(0));
    UINT m = ARG(1);
    /* Same window and message as the one in flight: this is the ordinary
     * "not mine, you take it" hand-back, so give the real lParam rather than
     * the truncated copy the game is holding. */
    LPARAM lp = (h == g_cur_msg.hwnd && m == g_cur_msg.msg)
                    ? g_cur_msg.lp : (LPARAM)(int32_t)ARG(3);
    RET((uint32_t)(LONG_PTR)DefWindowProcA(h, m, (WPARAM)ARG(2), lp));
    STDRET(4);
}

void imp_DestroyWindow(void) { RET(DestroyWindow(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_ShowWindow(void)    { RET(ShowWindow(HWND_OF(ARG(0)), (int)ARG(1)) ? 1 : 0); STDRET(2); }
void imp_SetFocus(void)      { RET(h_put(SetFocus(HWND_OF(ARG(0))))); STDRET(1); }
void imp_SetForegroundWindow(void) { RET(SetForegroundWindow(HWND_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_EnableWindow(void)  { RET(EnableWindow(HWND_OF(ARG(0)), (BOOL)ARG(1)) ? 1 : 0); STDRET(2); }
void imp_SetWindowTextA(void){ RET(SetWindowTextA(HWND_OF(ARG(0)), (LPCSTR)simp(ARG(1))) ? 1 : 0); STDRET(2); }
void imp_PostQuitMessage(void) { PostQuitMessage((int)ARG(0)); STDRET(1); }
void imp_MessageBeep(void)   { RET(MessageBeep(ARG(0)) ? 1 : 0); STDRET(1); }
void imp_ShowCursor(void)    { RET((uint32_t)ShowCursor((BOOL)ARG(0))); STDRET(1); }
void imp_SetCapture(void)    { RET(h_put(SetCapture(HWND_OF(ARG(0))))); STDRET(1); }
void imp_ReleaseCapture(void){ RET(ReleaseCapture() ? 1 : 0); STDRET(0); }
void imp_GetSystemMetrics(void) { RET((uint32_t)GetSystemMetrics((int)ARG(0))); STDRET(1); }
/*
 * Scripted input.
 *
 * MCG_CLICK="x,y@t;x,y@t;..." clicks engine point (x,y) t seconds after start.
 *
 * Driving the menus from outside does not work: the engine reads the pointer
 * with GetCursorPos and the buttons with GetAsyncKeyState, and a posted
 * WM_LBUTTONDOWN touches neither. Real synthetic input does, but only reaches
 * the window when it is the foreground one, which a background process cannot
 * reliably arrange -- every attempt landed on whatever was in front instead.
 * Feeding the two functions the engine actually polls sidesteps the whole
 * question, and makes the UI testable without a human at the keyboard.
 *
 * Coordinates are the engine's own screen, NOT the window's client, so they
 * come straight back out of a widget dump.
 */
#define CLICK_HOLD_MS   220      /* button held this long for a click */
#define DRAG_HOLD_MS    900      /* ...and this long for a drag */
#define CLICK_HOVER_MS  600      /* pointer parked this long either side */

static struct { int x, y, x2, y2, drag; unsigned at_ms; } g_clicks[8];
static int g_click_n = -1;
static DWORD g_start_ms;

static void clicks_init(void) {
    const char* s = getenv("MCG_CLICK");
    g_click_n = 0;
    g_start_ms = 0;            /* set when the logistics shell comes up */
    if (!s) return;
    while (*s && g_click_n < 8) {
        int x, y, x2, y2; double t;
        if (sscanf(s, "%d,%d>%d,%d@%lf", &x, &y, &x2, &y2, &t) == 5) {
            g_clicks[g_click_n].drag = 1;
        } else if (sscanf(s, "%d,%d@%lf", &x, &y, &t) == 3) {
            g_clicks[g_click_n].drag = 0;
            x2 = x; y2 = y;
        } else {
            break;
        }
        g_clicks[g_click_n].x = x;   g_clicks[g_click_n].y = y;
        g_clicks[g_click_n].x2 = x2; g_clicks[g_click_n].y2 = y2;
        g_clicks[g_click_n].at_ms = (unsigned)(t * 1000.0);
        if (g_clicks[g_click_n].drag)
            fprintf(stderr, "[input] scripted drag %d: (%d,%d) -> (%d,%d) at %.1fs\n",
                    g_click_n, x, y, x2, y2, t);
        else
            fprintf(stderr, "[input] scripted click %d: (%d,%d) at %.1fs\n",
                    g_click_n, x, y, t);
        g_click_n++;
        s = strchr(s, ';');
        if (!s) break;
        s++;
    }
}

/*
 * 0 = nothing scripted now, 1 = pointer parked there, 2 = and button down.
 *
 * A drag holds the button across a move, so the pointer walks from the source
 * to the target in a few steps while it is down -- a widget that only notices
 * the grab on a move would see nothing if it teleported.
 */
static int click_now(int* x, int* y) {
    if (g_click_n < 0) clicks_init();
    /*
     * Times are measured from the moment the logistics shell comes up, not from
     * process start. Startup speed varies by tens of seconds -- the intro-movie
     * sequence is the culprit -- so a fixed wall-clock offset landed on the main
     * menu only about half the time, and a missed first click wastes the whole
     * run. Mission::run's state reaching 3 is the engine saying "the shell is
     * ready", which is exactly the thing worth waiting for.
     */
    {
        uint32_t mission = MEM32(0x008087A8u);
        if (!g_start_ms) {
            if (!mission || MEM32(mission) != 3) return 0;
            g_start_ms = GetTickCount();
            fprintf(stderr, "[input] logistics shell up; script starts now\n");
        }
    }
    unsigned now = GetTickCount() - g_start_ms;
    for (int i = 0; i < g_click_n; i++) {
        unsigned at = g_clicks[i].at_ms;
        unsigned hold = g_clicks[i].drag ? DRAG_HOLD_MS : CLICK_HOLD_MS;
        if (now + CLICK_HOVER_MS < at || now > at + hold + CLICK_HOVER_MS)
            continue;
        if (now < at) {                       /* hovering over the source */
            *x = g_clicks[i].x;
            *y = g_clicks[i].y;
            return 1;
        }
        if (now >= at + hold) {               /* settled on the target */
            *x = g_clicks[i].x2;
            *y = g_clicks[i].y2;
            return 1;
        }
        {   /* button down: hold the source briefly, then walk to the target */
            unsigned into = now - at;
            unsigned grab = hold / 4;
            if (into < grab || !g_clicks[i].drag) {
                *x = g_clicks[i].x;
                *y = g_clicks[i].y;
            } else {
                unsigned num = into - grab, den = hold - grab;
                *x = g_clicks[i].x + (int)((g_clicks[i].x2 - g_clicks[i].x) * (int)num / (int)den);
                *y = g_clicks[i].y + (int)((g_clicks[i].y2 - g_clicks[i].y) * (int)num / (int)den);
            }
        }
        return 2;
    }
    return 0;
}

void imp_GetAsyncKeyState(void) {
    int x, y;
    if ((int)ARG(0) == VK_LBUTTON && click_now(&x, &y) == 2) {
        static int said;
        if (!said) { said = 1; fprintf(stderr, "[input] left button down\n"); }
        RET(0x8000u);
        STDRET(1);
        return;
    }
    RET((uint32_t)(uint16_t)GetAsyncKeyState((int)ARG(0)));
    STDRET(1);
}
void imp_GetKeyState(void)   { RET((uint32_t)(uint16_t)GetKeyState((int)ARG(0))); STDRET(1); }
void imp_GetCaretBlinkTime(void) { RET(GetCaretBlinkTime()); STDRET(0); }
void imp_GetMessagePos(void) { RET(GetMessagePos()); STDRET(0); }
void imp_RegisterWindowMessageA(void) { RET(RegisterWindowMessageA((LPCSTR)simp(ARG(0)))); STDRET(1); }
void imp_FindWindowA(void)   { RET(h_put(FindWindowA((LPCSTR)simp(ARG(0)), (LPCSTR)simp(ARG(1))))); STDRET(2); }
void imp_PostMessageA(void)  { RET(PostMessageA(HWND_OF(ARG(0)), ARG(1), (WPARAM)ARG(2), (LPARAM)(int32_t)ARG(3)) ? 1 : 0); STDRET(4); }
void imp_WaitMessage(void)   { RET(WaitMessage() ? 1 : 0); STDRET(0); }

/* RECT and POINT are fixed-width all through, so they pass straight across. */
void imp_SetRect(void)       { RET(SetRect((LPRECT)simp(ARG(0)), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)) ? 1 : 0); STDRET(5); }
void imp_PtInRect(void)      { POINT p = { (LONG)ARG(1), (LONG)ARG(2) }; RET(PtInRect((const RECT*)simp(ARG(0)), p) ? 1 : 0); STDRET(3); }
void imp_IntersectRect(void) { RET(IntersectRect((LPRECT)simp(ARG(0)), (const RECT*)simp(ARG(1)), (const RECT*)simp(ARG(2))) ? 1 : 0); STDRET(3); }
void imp_AdjustWindowRect(void) { RET(AdjustWindowRect((LPRECT)simp(ARG(0)), ARG(1), (BOOL)ARG(2)) ? 1 : 0); STDRET(3); }
void imp_ClientToScreen(void){ RET(ClientToScreen(HWND_OF(ARG(0)), (LPPOINT)simp(ARG(1))) ? 1 : 0); STDRET(2); }
void imp_GetCursorPos(void) {
    POINT p = { 0, 0 };
    BOOL ok = TRUE;
    int sx, sy;
    if (click_now(&sx, &sy)) {            /* scripted: already engine space */
        p.x = sx;
        p.y = sy;
    } else {
        ok = GetCursorPos(&p);
        ddraw_map_cursor(&p);             /* desktop -> the engine's screen */
    }
    { LPPOINT out = (LPPOINT)simp(ARG(0)); if (out) *out = p; }
    RET(ok ? 1 : 0);
    STDRET(1);
}

/* Both of these speak the engine's fullscreen coordinates, which are not the
 * desktop's. Confining or warping the host pointer with them would move it
 * somewhere the player did not ask for, and neither is load-bearing: the game
 * uses them to keep the cursor inside a screen that, windowed, it does not
 * own. */
void imp_ClipCursor(void)    { RET(1); STDRET(1); }
void imp_SetCursorPos(void)  { RET(1); STDRET(2); }
void imp_MapWindowPoints(void) { RET((uint32_t)MapWindowPoints(HWND_OF(ARG(0)), HWND_OF(ARG(1)), (LPPOINT)simp(ARG(2)), ARG(3))); STDRET(4); }
void imp_InvalidateRect(void) { RET(InvalidateRect(HWND_OF(ARG(0)), (const RECT*)simp(ARG(1)), (BOOL)ARG(2)) ? 1 : 0); STDRET(3); }

void imp_SetWindowPos(void) {
    RET(SetWindowPos(HWND_OF(ARG(0)), HWND_OF(ARG(1)), (int)ARG(2), (int)ARG(3),
                     (int)ARG(4), (int)ARG(5), ARG(6)) ? 1 : 0);
    STDRET(7);
}

void imp_SetWindowLongA(void) {
    /* GWL_WNDPROC would hand out a host function pointer; nothing else here is
     * translated, so pass the rest through and refuse that one loudly. */
    /* -4 is GWL_WNDPROC. Spelled numerically because the 64-bit headers only
     * define GWLP_WNDPROC, while the value the 32-bit game passes is -4. */
    if ((int)ARG(1) == -4) {
        fprintf(stderr, "[win] SetWindowLongA(GWL_WNDPROC) not supported\n");
        RET(0);
    } else {
        RET((uint32_t)SetWindowLongA(HWND_OF(ARG(0)), (int)ARG(1), (LONG)ARG(2)));
    }
    STDRET(3);
}

/* MSG, 32-bit: 28 bytes. hwnd is the only field that needs translating. */
#define MSG_HWND 0
#define MSG_MSG  4
#define MSG_WP   8
#define MSG_LP   12
#define MSG_TIME 16
#define MSG_PTX  20
#define MSG_PTY  24

static void msg_out(uint32_t dst, const MSG* m) {
    if (!dst) return;
    MEM32(dst + MSG_HWND) = h_put(m->hwnd);
    MEM32(dst + MSG_MSG)  = m->message;
    MEM32(dst + MSG_WP)   = (uint32_t)m->wParam;
    MEM32(dst + MSG_LP)   = (uint32_t)m->lParam;
    MEM32(dst + MSG_TIME) = m->time;
    MEM32(dst + MSG_PTX)  = (uint32_t)m->pt.x;
    MEM32(dst + MSG_PTY)  = (uint32_t)m->pt.y;
}

static void msg_in(uint32_t src, MSG* m) {
    memset(m, 0, sizeof *m);
    if (!src) return;
    m->hwnd    = HWND_OF(MEM32(src + MSG_HWND));
    m->message = MEM32(src + MSG_MSG);
    m->wParam  = (WPARAM)MEM32(src + MSG_WP);
    m->lParam  = (LPARAM)(int32_t)MEM32(src + MSG_LP);
    m->time    = MEM32(src + MSG_TIME);
    m->pt.x    = (LONG)MEM32(src + MSG_PTX);
    m->pt.y    = (LONG)MEM32(src + MSG_PTY);
}

/* ============================================================
 * The multimedia timer
 *
 * timeSetEvent returning 0 means "could not create the timer", and the engine
 * drives its heartbeat from one -- so with the stub in place its state machine
 * never ticked and it sat flipping a blank screen.
 *
 * The callback is run from the message pump on the MAIN thread rather than
 * from a real timer thread. That is deliberate: the recomp's registers and
 * stack are global, so re-entering lifted code from another thread would
 * corrupt whatever the main thread was in the middle of. A periodic callback
 * serviced a little late is a far better trade than that.
 *
 * ponytail: serviced from PeekMessage, so the resolution is the frame rate.
 * Fine for a heartbeat; a real timer thread would need the CPU state made
 * per-thread first.
 * ============================================================ */

static uint32_t g_timer_proc, g_timer_user, g_timer_period, g_timer_id;
static DWORD g_timer_last;

void imp_timeGetDevCaps(void) {
    uint32_t p = ARG(0);
    if (p && ARG(1) >= 8) { MEM32(p) = 1; MEM32(p + 4) = 1000000; }
    RET(0);                                  /* TIMERR_NOERROR */
    STDRET(2);
}

void imp_timeSetEvent(void) {
    g_timer_period = ARG(0) ? ARG(0) : 1;
    g_timer_proc = ARG(2);
    g_timer_user = ARG(3);
    g_timer_last = GetTickCount();
    g_timer_id = 1;
    fprintf(stderr, "[time] timeSetEvent(%u ms, proc=0x%08X) -> id %u\n",
            g_timer_period, g_timer_proc, g_timer_id);
    RET(g_timer_id);
    STDRET(5);
}

void imp_timeKillEvent(void) {
    g_timer_proc = 0;
    g_timer_id = 0;
    RET(0);
    STDRET(1);
}

void imp_timeGetSystemTime(void) {
    if (ARG(0)) MEM32(ARG(0)) = GetTickCount();   /* MMTIME: wType, u.ms */
    RET(0);
    STDRET(2);
}

/* Called from the pump. Re-entrancy is guarded: the callback can itself pump. */
static void service_timer(void) {
    static int inside;
    if (!g_timer_proc || inside) return;
    DWORD now = GetTickCount();
    if (now - g_timer_last < g_timer_period) return;
    g_timer_last = now;
    inside = 1;
    { static int shown; if (shown < 3) { shown++;
        fprintf(stderr, "[time] heartbeat firing\n"); } }
    /* TimeProc(uTimerID, uMsg, dwUser, dw1, dw2), stdcall. */
    uint32_t args[5] = { g_timer_id, 0, g_timer_user, 0, 0 };
    call_lifted(g_timer_proc, 5, args);
    inside = 0;
}

/* Counters the watchdog prints, so "parked" can be told apart from "spinning":
 * a PC that never moves means nothing at all is happening, but a PC that never
 * moves WHILE these climb means a loop that keeps landing on the same
 * instruction. */
volatile uint32_t g_n_peek, g_n_dispatch, g_n_wndproc;

void imp_PeekMessageA(void) {
    g_n_peek++;
    service_timer();
    MSG m;
    BOOL got = PeekMessageA(&m, HWND_OF(ARG(1)), ARG(2), ARG(3), ARG(4));
    if (got) msg_out(ARG(0), &m);
    RET(got ? 1 : 0);
    STDRET(5);
}

void imp_TranslateMessage(void) {
    MSG m; msg_in(ARG(0), &m);
    RET(TranslateMessage(&m) ? 1 : 0);
    STDRET(1);
}

void imp_DispatchMessageA(void) {
    g_n_dispatch++;
    MSG m; msg_in(ARG(0), &m);
    RET((uint32_t)(LONG_PTR)DispatchMessageA(&m));
    STDRET(1);
}

/* ============================================================
 * GDI. The imports here are exactly an 8-bit DIB blitter --
 * CreateCompatibleDC + CreateDIBSection + SelectObject + BitBlt +
 * SetDIBColorTable -- which is the engine's windowed presentation path.
 * ============================================================ */

void imp_GetDC(void)      { RET(h_put(GetDC(HWND_OF(ARG(0))))); STDRET(1); }
void imp_ReleaseDC(void)  { RET((uint32_t)ReleaseDC(HWND_OF(ARG(0)), HDC_OF(ARG(1)))); STDRET(2); }
void imp_CreateCompatibleDC(void) { RET(h_put(CreateCompatibleDC(HDC_OF(ARG(0))))); STDRET(1); }
void imp_DeleteDC(void)   { RET(DeleteDC(HDC_OF(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_DeleteObject(void) { RET(DeleteObject((HGDIOBJ)h_ptr(ARG(0))) ? 1 : 0); STDRET(1); }
void imp_SelectObject(void) { RET(h_put(SelectObject(HDC_OF(ARG(0)), (HGDIOBJ)h_ptr(ARG(1))))); STDRET(2); }
void imp_GetStockObject(void) { RET(h_put(GetStockObject((int)ARG(0)))); STDRET(1); }
void imp_SetStretchBltMode(void) { RET((uint32_t)SetStretchBltMode(HDC_OF(ARG(0)), (int)ARG(1))); STDRET(2); }
void imp_LoadIconA(void)  { RET(h_put(LoadIconA(NULL, IDI_APPLICATION))); STDRET(2); }

void shims_sync_dibs(void);

void imp_BitBlt(void) {
    { static int shown; if (shown < 3) { shown++;
        fprintf(stderr, "[gdi] BitBlt %dx%d -- the GDI display path is in use\n",
                (int)ARG(3), (int)ARG(4)); } }
    shims_sync_dibs();     /* the engine drew into the arena, not into the DIB */
    RET(BitBlt(HDC_OF(ARG(0)), (int)ARG(1), (int)ARG(2), (int)ARG(3),
               (int)ARG(4), HDC_OF(ARG(5)), (int)ARG(6), (int)ARG(7),
               ARG(8)) ? 1 : 0);
    STDRET(9);
}

/*
 * The pixels have to land in SIMULATED memory: the engine draws straight into
 * the pointer this returns, and that pointer is a 32-bit VA on its side. So the
 * bits are allocated from the arena and handed to CreateDIBSection as the
 * section offset... which it cannot do. Instead the DIB owns host memory and
 * the game gets a simulated alias, with the two kept in step at blit time.
 *
 * ponytail: one arena buffer per DIB, copied to the DIB's own bits on BitBlt.
 * A copy per frame of a 640x480 8-bit surface is 300 KB -- invisible next to
 * everything else here. Map the arena page directly if it ever shows up.
 */
#define MAX_DIBS 8
static struct { HBITMAP bmp; void* host_bits; uint32_t sim_bits; uint32_t bytes; }
    g_dibs[MAX_DIBS];
static uint32_t g_dib_n;

void imp_CreateDIBSection(void) {
    uint32_t bmi = ARG(1);
    void* bits = NULL;
    HBITMAP b = CreateDIBSection(HDC_OF(ARG(0)), (const BITMAPINFO*)simp(bmi),
                                 ARG(2), &bits, NULL, ARG(5));
    if (!b || !bits) {
        fprintf(stderr, "[gdi] CreateDIBSection failed (%lu)\n", GetLastError());
        if (ARG(3)) MEM32(ARG(3)) = 0;
        RET(0);
        STDRET(6);
        return;
    }
    /* Size from the header the caller just supplied. */
    int32_t w = (int32_t)MEM32(bmi + 4), h = (int32_t)MEM32(bmi + 8);
    uint32_t bpp = MEM32(bmi + 12) >> 16;
    if (h < 0) h = -h;
    uint32_t stride = (((uint32_t)w * bpp + 31) / 32) * 4;
    uint32_t bytes = stride * (uint32_t)h;
    uint32_t sim = arena_alloc(bytes ? bytes : 4);

    if (g_dib_n < MAX_DIBS) {
        g_dibs[g_dib_n].bmp = b;
        g_dibs[g_dib_n].host_bits = bits;
        g_dibs[g_dib_n].sim_bits = sim;
        g_dibs[g_dib_n].bytes = bytes;
        g_dib_n++;
    }
    fprintf(stderr, "[gdi] CreateDIBSection %dx%d %ubpp -> sim bits 0x%08X (%u bytes)\n",
            w, h, bpp, sim, bytes);
    if (ARG(3)) MEM32(ARG(3)) = sim;      /* the game draws here */
    RET(h_put(b));
    STDRET(6);
}

/* Where the engine's own DIB lives, so the DirectDraw present can say whether
 * the drawing went there instead of into the surface it flips. */
uint32_t shims_dib_bits(uint32_t* bytes) {
    if (!g_dib_n) return 0;
    if (bytes) *bytes = g_dibs[0].bytes;
    return g_dibs[0].sim_bits;
}

/* Push whatever the engine drew into the DIB the host will actually blit. */
void shims_sync_dibs(void) {
    for (uint32_t i = 0; i < g_dib_n; i++)
        if (g_dibs[i].sim_bits && g_dibs[i].bytes)
            memcpy(g_dibs[i].host_bits, (const void*)ADDR(g_dibs[i].sim_bits),
                   g_dibs[i].bytes);
}

void imp_SetDIBColorTable(void) {
    shims_sync_dibs();
    RET(SetDIBColorTable(HDC_OF(ARG(0)), ARG(1), ARG(2),
                         (const RGBQUAD*)simp(ARG(3))));
    STDRET(4);
}

void imp_CreatePalette(void)  { RET(h_put(CreatePalette((const LOGPALETTE*)simp(ARG(0))))); STDRET(1); }
void imp_SelectPalette(void)  { RET(h_put(SelectPalette(HDC_OF(ARG(0)), (HPALETTE)h_ptr(ARG(1)), (BOOL)ARG(2)))); STDRET(3); }
void imp_RealizePalette(void) { RET(RealizePalette(HDC_OF(ARG(0)))); STDRET(1); }
void imp_AnimatePalette(void) { RET(AnimatePalette((HPALETTE)h_ptr(ARG(0)), ARG(1), ARG(2), (const PALETTEENTRY*)simp(ARG(3))) ? 1 : 0); STDRET(4); }
void imp_GetSystemPaletteEntries(void) { RET(GetSystemPaletteEntries(HDC_OF(ARG(0)), ARG(1), ARG(2), (LPPALETTEENTRY)simp(ARG(3)))); STDRET(4); }

/*
 * SmackOpen -- a movie that is already over.
 *
 * A Smack begins with Version, Width, Height, Frames and MSPerFrame, and the
 * engine reads the width and height straight back out with no null check at
 * all: BriefingScreen::setUpOperation calls this and then does
 * `mov eax, [edi + 8]` on the result. So the generated stub's NULL is not a
 * safe answer -- it faults the instant a briefing starts.
 *
 * Hand back a real, zeroed structure describing a one-frame movie that is
 * already on its last frame. Every "is it finished yet" the engine asks then
 * answers yes, the briefing window gets a sane size, and no pixel is ever
 * decoded. One buffer serves every open, because nothing reads it back.
 *
 * ponytail: no decoder. The .smk files are on the disc and decoding them is a
 * project of its own; this is only what gets the game past the movies.
 */
void imp_SmackOpen(void) {
    static uint32_t smk;
    if (!smk) {
        smk = shims_alloc(1024);            /* far more than the fields used */
        memset((void*)ADDR(smk), 0, 1024);
        MEM32(smk +  0) = 0x324B4D53u;      /* "SMK2" */
        MEM32(smk +  4) = 320;              /* Width      */
        MEM32(smk +  8) = 200;              /* Height     */
        MEM32(smk + 12) = 1;                /* Frames     */
        MEM32(smk + 16) = 33;               /* MSPerFrame */
    }
    { static int said; if (!said) { said = 1;
        fprintf(stderr, "[smack] SmackOpen -> an empty 320x200 movie\n"); } }
    RET(smk);
    STDRET(3);
}

/* ============================================================
 * The registry, forwarded to the host's. MechCommander keeps its settings in
 * HKCU like any other Win32 app, so there is nothing to emulate -- only the
 * handle to translate. The predefined roots (HKEY_CURRENT_USER and friends)
 * are the same 32-bit constants on both sides, so they pass straight through.
 * ============================================================ */

#define H_KEY_TAG  0xF4E70000u
static HKEY g_keys[H_MAX];

/* Our own tag is tested FIRST. The predefined roots are 0x8000000x, and the
 * tag is above that too, so an "is it predefined?" test written as
 * `v >= 0x80000000` swallows every handle this table hands out -- which then
 * gets cast straight to a pointer and passed to the real registry API. That
 * only shows up on the second level of key opening, where the parent is one of
 * ours rather than a root. */
static HKEY map_hkey(uint32_t v) {
    if ((v & H_TAG_MASK) == H_KEY_TAG) {
        uint32_t i = v & 0xFFFFu;
        return i < H_MAX ? g_keys[i] : NULL;
    }
    if (v >= 0x80000000u && v <= 0x8000000Fu)
        return (HKEY)(ULONG_PTR)v;                          /* predefined root */
    return NULL;
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

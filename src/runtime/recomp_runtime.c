/*
 * MechCommander Gold recompilation - runtime core.
 *
 * Owns the simulated machine the lifted code runs inside: the register file, the
 * memory image, the stack, and the lookup paths RECOMP_ICALL/ITAIL use to turn a
 * 32-bit VA back into a C function.
 *
 * Memory model: MCX.EXE is mapped at its real image base by the shared loader,
 * so g_mem_base stays 0 and every MEM32(va) is a direct read of the program's
 * own data. That requires the host executable to be linked at a high base so the
 * target's range is free -- see CMakeLists.
 *
 * That base is 0x00600000, not the usual 0x00400000. It matters twice: the host
 * must leave a different range clear, and there is 6 MB of address space BELOW
 * the image rather than 4 MB. The stack still goes above it -- deriving the base
 * from the measured span is what keeps this correct if the image ever changes --
 * but the room underneath is there if a fixed low address is ever wanted.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "imports.h"
#include "image_loader.h"
#include "crash_report.h"

/* ============================================================
 * Register file
 * ============================================================ */

uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
uint32_t g_ebx = 0, g_esi = 0, g_edi = 0, g_ebp = 0;
uint16_t g_seg_cs = 0, g_seg_ds = 0, g_seg_es = 0;
uint16_t g_seg_fs = 0, g_seg_gs = 0, g_seg_ss = 0;
uint32_t g_fs_base = 0, g_gs_base = 0;

double   g_st[8]   = {0};
int      g_fp_top  = 0;
uint16_t g_fpu_cw  = 0x037F;   /* x87 default: round-to-nearest, 64-bit mantissa */
uint64_t g_mm[8]   = {0};

ptrdiff_t g_mem_base = 0;      /* 1:1 mapping -- see the note above */

uint32_t g_cur_func = 0;
uint32_t g_icall_trace[ICALL_TRACE_SIZE] = {0};
uint32_t g_icall_trace_idx = 0;
uint32_t g_icall_count = 0;

#ifdef RECOMP_TRACE
uint32_t g_enter_trace[RECOMP_ENTER_SIZE] = {0};
uint32_t g_enter_idx = 0;
/* VA of the instruction currently executing. The generated code assigns this
 * before every instruction under RECOMP_TRACE, which turns "it faulted
 * somewhere in this 777-byte function" into an exact address. */
uint32_t g_cur_pc = 0;
#endif

/* Everything worth printing when something faults, in one place. */
static void report_extra(void) {
#ifdef RECOMP_TRACE
    fprintf(stderr, "  faulting instruction: 0x%08X\n", g_cur_pc);
#endif
    shims_report();
}

/* ============================================================
 * Memory layout
 * ============================================================ */

#define MCG_IMAGE_BASE  0x00600000u
#define MCG_ENTRY       0x00762C24u   /* PE entry point (MSVC 6 CRT startup) */
/* `___sbh_threshold` in .data, named by the binary's own symbol table. Ships
 * holding 0x3F8, so every allocation up to 1016 bytes goes to the small-block
 * heap. See the note at the write in main(). */
#define MCG_SBH_THRESHOLD 0x007A714Cu

#define STACK_SIZE         0x00400000u   /* 4 MB */
#define ALLOC_GRANULARITY  0x00010000u

static uint32_t g_stack_base = 0;
static uint32_t g_stack_top = 0;
static uint32_t g_image_span = 0;

uint32_t recomp_image_base(void) { return MCG_IMAGE_BASE; }
uint32_t recomp_image_end(void)  { return MCG_IMAGE_BASE + g_image_span; }
uint32_t recomp_stack_base(void) { return g_stack_base; }
uint32_t recomp_stack_end(void)  { return g_stack_top; }

#ifdef RECOMP_TRACE
void recomp_trace_enter(uint32_t va) {
    g_enter_trace[g_enter_idx & (RECOMP_ENTER_SIZE - 1)] = va;
    g_enter_idx++;
}
#endif

void recomp_dump_trace(const char* why) {
#ifdef RECOMP_TRACE
    (void)why;
    fprintf(stderr, "  last %d function entries (oldest first):\n", RECOMP_ENTER_SIZE);
    for (int i = 0; i < RECOMP_ENTER_SIZE; i++) {
        uint32_t idx = (g_enter_idx - RECOMP_ENTER_SIZE + i) & (RECOMP_ENTER_SIZE - 1);
        if (g_enter_trace[idx])
            fprintf(stderr, "    sub_%08X\n", g_enter_trace[idx]);
    }
#else
    (void)why;
#endif
}

/* Turn a simulated VA into a region name for the crash report. A bad pointer's
 * region is usually the whole diagnosis: "read of 00000000" is a null deref,
 * "read of 00C20004 (heap arena)" is a use of freed or uninitialised memory. */
static const char* describe_region(uint32_t va) {
    if (va < 0x10000u)                          return "(null page)";
    if (va >= MCG_IMAGE_BASE && va < MCG_IMAGE_BASE + g_image_span)
                                                return "(image)";
    if (va >= g_stack_base && va < g_stack_top) return "(stack)";
    if (va >= shims_arena_base() && va < shims_arena_end())
                                                return "(heap arena)";
    return "(unmapped)";
}

/* Place the stack immediately above the mapped image. Deriving the base from
 * the measured span means it cannot silently overlap however the image grows. */
static int map_stack(uint32_t image_base, uint32_t span) {
    uint32_t base = (image_base + span + ALLOC_GRANULARITY - 1)
                    & ~(ALLOC_GRANULARITY - 1);
    void* p = VirtualAlloc((void*)(uintptr_t)base, STACK_SIZE,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        fprintf(stderr, "[runtime] stack VirtualAlloc @ 0x%08X (%u MB) failed (%lu)\n",
                base, STACK_SIZE / 1048576u, GetLastError());
        return 0;
    }
    memset(p, 0, STACK_SIZE);
    g_stack_base = base;
    g_stack_top = base + STACK_SIZE;
    fprintf(stderr, "[runtime] stack 0x%08X-0x%08X\n", base, g_stack_top);
    return 1;
}

/* ============================================================
 * Dispatch
 * ============================================================ */

/* Binary search over the generated table, which run_pipeline.py emits sorted. */
recomp_func_t recomp_lookup(uint32_t va) {
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t k = recomp_dispatch_table[mid].address;
        if (k == va) return recomp_dispatch_table[mid].func;
        if (k < va) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Hand-installed overrides, for bisecting a bad lift during bring-up: point one
 * VA at a hand-written body without regenerating anything. Empty by design. */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

/* Import bridges. 204 entries, generated in IAT order, so a linear scan.
 * ponytail: linear over 204, sort + bisect if it ever shows up in a profile. */
/*
 * Synthetic VAs for runtime-resolved exports.
 *
 * GetProcAddress has to hand back something the game can `call`, but a
 * LoadLibrary'd DLL has no IAT slot to point at. So each implemented export
 * gets an address from a range that is deliberately above every mapped region
 * -- image, stack and arena all sit below 0x10C20000 -- which means a stray
 * pointer into this range is unmistakable rather than plausible.
 */
#define DYN_VA_BASE  0x11000000u

uint32_t mcg_dyn_resolve(const char* module, const char* proc) {
    for (uint32_t i = 0; i < mcg_dyn_export_count; i++)
        if (!_stricmp(mcg_dyn_exports[i].module, module) &&
            !strcmp(mcg_dyn_exports[i].proc, proc))
            return DYN_VA_BASE + i * 4u;
    return 0;
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    for (uint32_t i = 0; i < mcg_import_bridge_count; i++)
        if (mcg_import_bridges[i].address == va)
            return mcg_import_bridges[i].func;
    if (va >= DYN_VA_BASE &&
        (va - DYN_VA_BASE) / 4u < mcg_dyn_export_count && !(va & 3u))
        return mcg_dyn_exports[(va - DYN_VA_BASE) / 4u].func;
    return NULL;
}

void mcg_install_iat(void) {
    for (uint32_t i = 0; i < mcg_iat_slot_count; i++) {
        uint32_t slot = mcg_iat_slots[i];
        MEM32(slot) = slot;
    }
}

/* ============================================================
 * Entry
 * ============================================================ */

int main(int argc, char** argv) {
    const char* image = (argc > 1) ? argv[1] : "MCX.EXE";

    uint32_t span = recomp_load_image(image, MCG_IMAGE_BASE);
    if (!span) {
        fprintf(stderr, "[runtime] could not map %s\n", image);
        return 1;
    }
    fprintf(stderr, "[runtime] mapped %s at 0x%08X, %.1f MB\n",
            image, MCG_IMAGE_BASE, span / 1048576.0);

    g_image_span = span;
    if (!map_stack(MCG_IMAGE_BASE, span)) return 1;

    /* Image / stack / heap are three disjoint regions climbing the low 32 bits
     * in a fixed order, so a stray pointer's region names itself. */
    if (!shims_init(g_stack_top)) return 1;

    /* fs: is thread-relative; the lifter emits FS_BASE + offset for it, so the
     * base has to point at a TIB before any lifted code touches fs:[0]. */
    g_fs_base = shims_make_tib(g_stack_base, g_stack_top);
    if (!g_fs_base) return 1;

    shims_set_program(image);

    recomp_set_region_describer(describe_region);
    recomp_set_extra_reporter(report_extra);
    recomp_install_crash_handler();

    mcg_install_iat();
    fprintf(stderr, "[runtime] %u lifted functions, %u import bridges\n",
            recomp_dispatch_count, mcg_import_bridge_count);

    g_esp = g_stack_top - 0x100;    /* a little headroom above the first frame */
    g_ebp = 0;

    /*
     * Route every allocation through HeapAlloc rather than the CRT's
     * small-block heap.
     *
     * `___sbh_threshold` is the CRT's own switch for this -- it is what
     * _set_sbh_threshold() writes, and __heap_alloc consults it on every call:
     * `cmp size, [___sbh_threshold] / ja` sends anything larger straight to
     * HeapAlloc. Zero sends everything there. Running with the small-block heap
     * disabled is a configuration MSVC 6 supports, not a patch to the game.
     *
     * It is also a sidestep, and worth being honest about: lifted
     * __sbh_alloc_block faults writing a block footer at an address built from
     * a local that holds a pointer where a size belongs. The lift of the
     * faulting instruction is faithful and so is the flag model around it, so
     * the bad value comes from somewhere earlier in 777 bytes of bitmap
     * arithmetic. That is a lot of debugging for an allocator optimisation with
     * no effect on game behaviour -- but it is unexplained, not fixed, and it
     * stays on the open list because whatever is wrong there could be wrong
     * elsewhere too.
     */
    MEM32(MCG_SBH_THRESHOLD) = 0;

    recomp_func_t entry = recomp_lookup(MCG_ENTRY);
    if (!entry) {
        fprintf(stderr, "[runtime] entry 0x%08X is not in the dispatch table\n",
                MCG_ENTRY);
        return 1;
    }

    fprintf(stderr, "[runtime] entering at 0x%08X\n", MCG_ENTRY);
    PUSH32(g_esp, RECOMP_RETADDR);
    entry();

    fprintf(stderr, "[runtime] entry returned; %u indirect calls\n", g_icall_count);
    return 0;
}

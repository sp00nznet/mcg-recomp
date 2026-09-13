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

/*
 * Call-site stack balance check (trace builds only).
 *
 * RECOMP_CALL pushes the dummy return address; the callee's `ret` pops it, plus
 * its arguments for a stdcall. So esp after a call is never lower than before
 * it. If it is, the callee never popped -- 4 bytes leak per call, and the
 * symptom surfaces much later as the poison value turning up in a register that
 * was supposed to be callee-saved. Reporting at the call site turns "something,
 * somewhere, earlier" into one address.
 *
 * Reports each offending site once, so a leak in a hot function says its name
 * instead of scrolling the log away.
 */
#define BALANCE_SEEN_MAX 64
static uint32_t g_balance_seen[BALANCE_SEEN_MAX];
static uint32_t g_balance_seen_n;

static int balance_first_time(uint32_t site) {
    for (uint32_t i = 0; i < g_balance_seen_n; i++)
        if (g_balance_seen[i] == site) return 0;
    if (g_balance_seen_n < BALANCE_SEEN_MAX)
        g_balance_seen[g_balance_seen_n++] = site;
    return 1;
}

/* ebp is callee-saved: a call must hand it back unchanged. This fails
 * differently from an esp drift and is worth its own check -- a zeroed ebp
 * turns every `[ebp - n]` into a small absolute address, which faults a long
 * way from whichever body actually lost it. */
static void check_bp(uint32_t bp_before, uint32_t site, uint32_t target) {
    if (g_ebp == bp_before || !balance_first_time(site))
        return;
    if (target)
        fprintf(stderr, "[ebp-clobber] indirect call at 0x%08X -> 0x%08X: "
                "ebp %08X -> %08X\n", site, target, bp_before, g_ebp);
    else
        fprintf(stderr, "[ebp-clobber] call at 0x%08X: ebp %08X -> %08X\n",
                site, bp_before, g_ebp);
}

/* Indirect form: the callee's VA is known here, so the report names the
 * function rather than only the dispatch site it went through. */
void recomp_check_balance2(uint32_t sp_before, uint32_t bp_before,
                           uint32_t site, uint32_t target) {
    check_bp(bp_before, site, target);
#if RECOMP_RETADDR
    if ((g_ebx == RECOMP_RETADDR || g_esi == RECOMP_RETADDR ||
         g_edi == RECOMP_RETADDR || g_ebp == RECOMP_RETADDR)
        && balance_first_time(site)) {
        fprintf(stderr, "[poison] after indirect call at 0x%08X -> 0x%08X: "
                "ebx=%08X esi=%08X edi=%08X ebp=%08X\n",
                site, target, g_ebx, g_esi, g_edi, g_ebp);
        return;
    }
#endif
    uint32_t low = sp_before - g_esp;
    if (g_esp < sp_before && low <= 16 && balance_first_time(site))
        fprintf(stderr, "[stack-leak] indirect call at 0x%08X -> 0x%08X left "
                        "esp %u bytes low\n", site, target, low);
}

void recomp_check_balance(uint32_t sp_before, uint32_t bp_before, uint32_t site) {
    check_bp(bp_before, site, 0);
    /* The poison is the real prize. RECOMP_RETADDR is a value no program would
     * compute, so finding it in a register the callee was supposed to preserve
     * names the exact call that lost it -- which beats discovering it later as
     * a nonsense pointer somewhere unrelated. */
#if RECOMP_RETADDR
    if ((g_ebx == RECOMP_RETADDR || g_esi == RECOMP_RETADDR ||
         g_edi == RECOMP_RETADDR || g_ebp == RECOMP_RETADDR)
        && balance_first_time(site)) {
        fprintf(stderr, "[poison] after call at 0x%08X: "
                "ebx=%08X esi=%08X edi=%08X ebp=%08X\n",
                site, g_ebx, g_esi, g_edi, g_ebp);
        return;
    }
#endif
    /* A dropped `ret` leaks one slot, occasionally a few. A large drop is a
     * callee that deliberately allocates for its caller -- __chkstk hands back
     * hundreds of bytes and is doing exactly its job -- so only small,
     * slot-sized shortfalls are worth reporting. */
    uint32_t low = sp_before - g_esp;
    if (g_esp < sp_before && low <= 16 && balance_first_time(site))
        fprintf(stderr, "[stack-leak] call at 0x%08X returned with esp %u bytes "
                        "low (%08X -> %08X)\n", site, low, sp_before, g_esp);
}

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

/*
 * Hand-installed overrides: point one VA at a hand-written body without
 * regenerating anything.
 *
 * The two here are the engine's own error reporters, from L:\mcx\Lib\aError.cpp.
 * They are intercepted not because their lift is wrong but because of what they
 * do on the way: both sprintf the message together with a float, and %f goes
 * through the CRT's x87 path ($I10_OUTPUT, __fltout, __fptostr), which reads an
 * 80-bit extended value's raw bytes. The lifter models the x87 stack as a 64-bit
 * C double, so that conversion cannot round-trip and the formatting derails
 * before the message is ever printed.
 *
 * The message is the point -- it is the game explaining what it could not find.
 * So take the char* and print it, and skip the float entirely.
 *
 * Both are __cdecl: the caller cleans the arguments, so the epilogue pops only
 * the dummy return address.
 */
static void mcg_report_msg(const char* kind) {
    uint32_t msg = MEM32(g_esp + 4);
    fprintf(stderr, "[game:%s] %s\n", kind,
            msg ? (const char*)ADDR(msg) : "(null)");
    g_esp += 4;
}

static void mcg_FatalMsg(void)   { mcg_report_msg("fatal"); }
static void mcg_GeneralMsg(void) { mcg_report_msg("msg"); }

/* void __cdecl Fatal(unsigned int code, char* a, char* b)
 *
 * Despite the name it RETURNS: it puts up a dialog and, unless the user asks to
 * break, carries on ("Could not find systemHeap. Using Defaults." is one of its
 * messages). So this reports and returns too -- exiting here would stop the
 * game somewhere the original would have kept going. */
static void mcg_Fatal(void) {
    uint32_t code = MEM32(g_esp + 4);
    uint32_t a = MEM32(g_esp + 8), b = MEM32(g_esp + 12);
    fprintf(stderr, "[game:Fatal] code=0x%08X  \"%s\"  \"%s\"\n", code,
            a ? (const char*)ADDR(a) : "", b ? (const char*)ADDR(b) : "");
    g_esp += 4;
}

/*
 * void __cdecl _initterm(void (**start)(void), void (**end)(void))
 *
 * Walks the C++ static-constructor table and calls each non-null entry. It is
 * replaced here as a PROBE, not because its lift is wrong: the constructors it
 * calls take no arguments and are __cdecl, so esp must come back exactly where
 * it started for every one of them. That is an invariant the generic call-site
 * check cannot assert -- for an arbitrary callee a raised esp is just a stdcall
 * cleaning up its arguments -- but here any change at all is a bug, and this
 * says which constructor caused it.
 *
 * That matters because something in this loop over-pops: `pop esi` in the real
 * _initterm retrieves the dummy return address instead of the saved esi.
 */
static void mcg_initterm(void) {
    uint32_t start = MEM32(g_esp + 4);
    uint32_t end = MEM32(g_esp + 8);
    uint32_t n = 0, bad = 0;

    for (uint32_t p = start; p < end; p += 4) {
        uint32_t fn = MEM32(p);
        if (!fn)
            continue;
        recomp_func_t f = recomp_lookup_manual(fn);
        if (!f) f = recomp_lookup(fn);
        if (!f) f = recomp_lookup_import(fn);
        if (!f) {
            fprintf(stderr, "[initterm] unresolved constructor 0x%08X\n", fn);
            continue;
        }
        uint32_t sp = g_esp;
        PUSH32(g_esp, RECOMP_RETADDR);
        f();
        n++;
        if (g_esp != sp) {
            bad++;
            fprintf(stderr, "[initterm] ctor 0x%08X left esp %+d (%08X -> %08X)\n",
                    fn, (int)(g_esp - sp), sp, g_esp);
            g_esp = sp;          /* keep going: report them all, not just one */
        }
    }
    fprintf(stderr, "[initterm] %u constructors, %u unbalanced\n", n, bad);
    g_esp += 4;                  /* __cdecl: the caller cleans the arguments */
}

/*
 * void __cdecl __fassign(int isDouble, char* dst, char* src)
 *
 * The CRT's string-to-float assignment, used by every scanf that reads a %f or
 * %lf. It delegates to __atodbl/__atoflt, which go through _strgtold12 and the
 * 12-byte internal long-double format -- a conversion the lifter's 64-bit
 * model of the x87 stack cannot represent, and which came back leaving ebp
 * zeroed, so the caller's next `[ebp - n]` read a small absolute address.
 *
 * strtod is the same conversion, exactly specified, and the host has one.
 */
static void mcg_fassign(void) {
    uint32_t is_double = MEM32(g_esp + 4);
    uint32_t dst = MEM32(g_esp + 8);
    uint32_t src = MEM32(g_esp + 12);
    double v = src ? strtod((const char*)ADDR(src), NULL) : 0.0;
    if (dst) {
        if (is_double) {
            memcpy((void*)ADDR(dst), &v, sizeof v);
        } else {
            float f = (float)v;
            memcpy((void*)ADDR(dst), &f, sizeof f);
        }
    }
    g_esp += 4;                      /* __cdecl: the caller cleans up */
}

/*
 * void __cdecl Assert(int condition, unsigned line, char* file, char* msg)
 *
 * The engine's own assertion. Overridden for two reasons: it reports what the
 * engine thinks is wrong, in its own words, which is the most direct diagnostic
 * available here; and the lifted body comes back with ebp changed, which then
 * corrupts its caller's frame. A failed assert is printed and execution
 * continues, matching the retail behaviour of a dialog the user dismisses.
 */
static void mcg_Assert(void) {
    uint32_t cond = MEM32(g_esp + 4);
    uint32_t line = MEM32(g_esp + 8);
    uint32_t file = MEM32(g_esp + 12);
    uint32_t msg = MEM32(g_esp + 16);
    if (!cond) {
        static uint32_t seen[32];
        static uint32_t n;
        uint32_t i = 0;
        for (; i < n; i++)
            if (seen[i] == line) break;
        if (i == n && n < 32) {          /* once per assertion site */
            seen[n++] = line;
            fprintf(stderr, "[game:Assert] %s:%u  %s\n",
                    file ? (const char*)ADDR(file) : "?", line,
                    msg ? (const char*)ADDR(msg) : "");
        }
    }
    g_esp += 4;                          /* __cdecl */
}

recomp_func_t recomp_lookup_manual(uint32_t va) {
    switch (va) {
    case 0x00644250u: return mcg_Assert;       /* Assert    */
    case 0x00760D27u: return mcg_initterm;     /* _initterm */
    case 0x00763EDAu: return mcg_fassign;      /* __fassign  */
    case 0x006440B0u: return mcg_GeneralMsg;   /* GeneralMsg(char*) */
    case 0x006440D0u: return mcg_FatalMsg;     /* FatalMsg(char*)   */
    case 0x00644180u: return mcg_Fatal;        /* Fatal(uint,char*,char*) */
    default:          return NULL;
    }
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

/* ============================================================
 * Lifted-CRT self-test
 *
 * When the engine reads a config file correctly and still cannot find a key
 * that is plainly in the buffer, the suspect is not the I/O -- it is one of the
 * string primitives the parser runs on top of. These are the CRT's hand-written
 * assembly versions (strlen reads four bytes at a time and finds the NUL with
 * carry propagation), so they lean on exactly the flag semantics a lifter is
 * most likely to get subtly wrong.
 *
 * So: call each lifted routine with known inputs and compare against the host's
 * answer for the same inputs. A failure names the function instead of leaving a
 * parser that "just doesn't work".
 * ============================================================ */

static uint32_t call_lifted(uint32_t va, int argc, const uint32_t* args) {
    recomp_func_t f = recomp_lookup(va);
    if (!f) {
        fprintf(stderr, "  [selftest] 0x%08X not in the dispatch table\n", va);
        return 0;
    }
    uint32_t save = g_esp;
    for (int i = argc - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, RECOMP_RETADDR);
    f();
    /* Restore rather than trust the callee's epilogue, so one bad convention
     * cannot cascade into the next case. */
    g_esp = save;
    return g_eax;
}

static int g_st_fail;

static void expect(const char* what, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "  [FAIL] %-34s got %08X  want %08X\n", what, got, want);
        g_st_fail++;
    } else {
        fprintf(stderr, "  [ ok ] %-34s %08X\n", what, got);
    }
}

static int sgn(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

static int crt_selftest(void) {
    /* Addresses come from MCX.EXE's own symbol table. */
    const uint32_t STRLEN = 0x0075FE10, STRCMP = 0x0075FE90;
    const uint32_t STRSTR = 0x00761570, STRNCMP = 0x00761FB0;
    const uint32_t MEMCMP = 0x007625D0, STRCHR = 0x007600A0;
    const uint32_t ATOL = 0x007602BD;

    /* A line shaped like the ones the config parser actually walks. */
    const char* line = "ul SymbolTableHeapSize = 1024000";
    uint32_t s_empty = shims_put_str("");
    uint32_t s_a = shims_put_str("a");
    uint32_t s_line = shims_put_str(line);
    uint32_t s_abl = shims_put_str("[ABL]");
    uint32_t s_hay = shims_put_str("\r\n[UseMusic]\r\n\r\n[ABL]\r\nul Symbol");
    uint32_t s_num = shims_put_str("1024000");

    fprintf(stderr, "=== lifted CRT self-test ===\n");

    uint32_t a[3];
    a[0] = s_empty; expect("strlen(\"\")", call_lifted(STRLEN, 1, a), 0);
    a[0] = s_a;     expect("strlen(\"a\")", call_lifted(STRLEN, 1, a), 1);
    a[0] = s_line;
    expect("strlen(config line)", call_lifted(STRLEN, 1, a),
           (uint32_t)strlen(line));

    a[0] = s_line; a[1] = s_line;
    expect("sgn strcmp(x,x)", (uint32_t)sgn((int)call_lifted(STRCMP, 2, a)), 0);
    a[0] = s_a; a[1] = s_line;
    expect("sgn strcmp(\"a\",line)",
           (uint32_t)sgn((int)call_lifted(STRCMP, 2, a)),
           (uint32_t)sgn(strcmp("a", line)));

    /* The one the parser lives on: find a section header inside a chunk. */
    a[0] = s_hay; a[1] = s_abl;
    {
        uint32_t got = call_lifted(STRSTR, 2, a);
        const char* hay = (const char*)ADDR(s_hay);
        const char* hit = strstr(hay, "[ABL]");
        expect("strstr(chunk, \"[ABL]\") offset",
               got ? got - s_hay : 0xFFFFFFFFu,
               hit ? (uint32_t)(hit - hay) : 0xFFFFFFFFu);
    }

    a[0] = s_line; a[1] = shims_put_str("ul Symbol"); a[2] = 9;
    expect("sgn strncmp(line,\"ul Symbol\",9)",
           (uint32_t)sgn((int)call_lifted(STRNCMP, 3, a)), 0);

    a[0] = s_line; a[1] = s_line; a[2] = (uint32_t)strlen(line);
    expect("memcmp(x,x,n)", call_lifted(MEMCMP, 3, a), 0);

    a[0] = s_line; a[1] = '=';
    {
        uint32_t got = call_lifted(STRCHR, 2, a);
        const char* hit = strchr(line, '=');
        expect("strchr(line,'=') offset", got ? got - s_line : 0xFFFFFFFFu,
               hit ? (uint32_t)(hit - line) : 0xFFFFFFFFu);
    }

    a[0] = s_num; expect("atol(\"1024000\")", call_lifted(ATOL, 1, a), 1024000);

    fprintf(stderr, "=== %s (%d failure%s) ===\n",
            g_st_fail ? "FAILURES" : "all passed", g_st_fail,
            g_st_fail == 1 ? "" : "s");
    return g_st_fail ? 1 : 0;
}

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

    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--selftest"))
            return crt_selftest();

    fprintf(stderr, "[runtime] entering at 0x%08X\n", MCG_ENTRY);
    PUSH32(g_esp, RECOMP_RETADDR);
    entry();

    fprintf(stderr, "[runtime] entry returned; %u indirect calls\n", g_icall_count);
    return 0;
}

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

/* A ring of (call site -> callee) pairs. The function-entry trace says WHAT
 * ran; this says who called it, which is the half you need when an object
 * arrives at a virtual method already wrong. */
#define CALLSITE_RING 32
static struct { uint32_t site, target; } g_callsites[CALLSITE_RING];
static uint32_t g_callsite_i;

static void note_callsite(uint32_t site, uint32_t target) {
    g_callsites[g_callsite_i & (CALLSITE_RING - 1)].site = site;
    g_callsites[g_callsite_i & (CALLSITE_RING - 1)].target = target;
    g_callsite_i++;
}

/* A shadow call STACK, pushed before each call and popped after it returns.
 *
 * The ring above only ever sees calls that came back, which makes it blind to
 * exactly the path that faults: every frame between the crash and whatever
 * started it is, by definition, still outstanding. This keeps those. */
#define CALLSTACK_MAX 256
static uint32_t g_callstack[CALLSTACK_MAX];
static uint32_t g_callstack_n;

/* The Ready Area's scroll pane, remembered when the briefing screen shows it,
 * so the watchdog can report its state live. Reading it inside setUpDeploy is
 * too early: the show happens afterwards, which is how "visible=0" was
 * mistaken for the bug once already. */
static uint32_t g_ready_pane;

/*
 * A watch on one simulated dword, checked at every call boundary.
 *
 * "Who wrote over this object?" is not answerable from a crash dump -- by the
 * time it faults the writer is long gone. Polling the value after every call
 * narrows the culprit to a single call site, which for a blitter smashing the
 * heap is all that is needed. Set MCG_WATCH=0xADDRESS to arm it.
 *
 * ponytail: a poll, not a hardware watchpoint. It names the call during which
 * the value changed rather than the instruction; a guard page would give the
 * instruction and can replace this if a call site is ever too coarse.
 */
static uint32_t g_watch_va, g_watch_val, g_watch_off;
static int g_watch_armed, g_watch_indirect;

/* The arena is reserved but committed on demand, so the address is usually not
 * mapped yet when the watch is requested. VirtualQuery answers that without
 * touching it -- probing under SEH instead raised an exception per call, and
 * the crash handler is a vectored one, so it reported every single one. */
static int watch_mapped(uint32_t va, uint32_t* out) {
    MEMORY_BASIC_INFORMATION mbi;
    void* p = (void*)(uintptr_t)ADDR(va);
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT)
        return 0;
    *out = MEM32(va);
    return 1;
}

static int watch_read(uint32_t va, uint32_t* out) {
    if (!g_watch_indirect)
        return watch_mapped(va, out);
    /* Indirect: the interesting field belongs to an object whose address is
     * only known at runtime, so follow the pointer each time. */
    uint32_t obj;
    if (!watch_mapped(va, &obj) || !obj) return 0;
    return watch_mapped(obj + g_watch_off, out);
}

/*
 * MCG_WATCH=0xADDR watches that dword.
 * MCG_WATCH=*0xADDR+0xOFF watches [[ADDR] + OFF] -- a field of an object the
 * program allocates, which is the usual case once past startup. Mission::run's
 * state machine, for instance, is `*0x8087A8+0`.
 */
static void watch_init(void) {
    const char* s = getenv("MCG_WATCH");
    if (!s) return;
    if (*s == '*') { g_watch_indirect = 1; s++; }
    g_watch_va = (uint32_t)strtoul(s, (char**)&s, 0);
    if (*s == '+') g_watch_off = (uint32_t)strtoul(s + 1, NULL, 0);
    fprintf(stderr, "[watch] arming on %s%08X+%X\n",
            g_watch_indirect ? "*" : "", g_watch_va, g_watch_off);
}

static void watch_check(uint32_t site) {
    uint32_t now;
    if (!g_watch_va || !watch_read(g_watch_va, &now)) return;
    if (!g_watch_armed) {                 /* first time it is mapped */
        g_watch_armed = 1;
        g_watch_val = now;
        fprintf(stderr, "[watch] %08X mapped, now %08X (at call 0x%08X)\n",
                g_watch_va, now, site);
        return;
    }
    if (now == g_watch_val) return;
    fprintf(stderr, "[watch] %08X changed %08X -> %08X across the call at "
                    "0x%08X\n", g_watch_va, g_watch_val, now, site);
    g_watch_val = now;
}

void recomp_push_site(uint32_t site) {
    if (g_callstack_n < CALLSTACK_MAX)
        g_callstack[g_callstack_n] = site;
    g_callstack_n++;
}

static void pop_site(void) {
    if (g_callstack_n) g_callstack_n--;
}

static void dump_callstack(void) {
    fprintf(stderr, "  call stack (outermost first, %u deep):\n", g_callstack_n);
    uint32_t n = g_callstack_n < CALLSTACK_MAX ? g_callstack_n : CALLSTACK_MAX;
    uint32_t from = n > 24 ? n - 24 : 0;       /* the innermost two dozen */
    for (uint32_t i = from; i < n; i++)
        fprintf(stderr, "    called from 0x%08X\n", g_callstack[i]);
}

/* Same walk, callable from a shim: "who asked for this?" is the whole question
 * when the engine repeats an expensive setup step and nothing says why. */
void recomp_dump_callstack(void) { dump_callstack(); }

static void dump_callsites(void) {
    fprintf(stderr, "  recent indirect call sites (oldest first):\n");
    for (uint32_t i = 0; i < CALLSITE_RING; i++) {
        uint32_t k = (g_callsite_i - CALLSITE_RING + i) & (CALLSITE_RING - 1);
        if (g_callsites[k].site)
            fprintf(stderr, "    0x%08X -> 0x%08X\n",
                    g_callsites[k].site, g_callsites[k].target);
    }
}

/* Indirect form: the callee's VA is known here, so the report names the
 * function rather than only the dispatch site it went through. */
void recomp_check_balance2(uint32_t sp_before, uint32_t bp_before,
                           uint32_t site, uint32_t target) {
    pop_site();
    watch_check(site);
    note_callsite(site, target);
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
    /* Direct calls are recorded too: a derived draw() calling its own base
     * class is a direct call, and that is exactly the edge where a `this`
     * arrives already wrong. */
    pop_site();
    watch_check(site);
    note_callsite(site, 0);
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

/* A bad pointer's CONTENTS usually say what it really is: a vtable pointer and
 * plausible fields mean a live object at the wrong offset; a repeating byte
 * means fill; readable text or palette indices mean it is pointing into data. */
static const char* describe_region(uint32_t va);

/* Via the accessors, not the layout constants: this sits above the section that
 * defines them. */
static int readable_va(uint32_t va) {
    return (va >= recomp_image_base() && va < recomp_image_end())
        || (va >= recomp_stack_base() && va < recomp_stack_end())
        || (va >= shims_arena_base() && va < shims_arena_end());
}

static void dump_around(const char* what, uint32_t va) {
    if (!readable_va(va) || !readable_va(va + 31))
        return;
    fprintf(stderr, "  %s = %08X %s:\n", what, va, describe_region(va));
    for (int row = 0; row < 2; row++) {
        fprintf(stderr, "    %08X:", va + row * 16);
        for (int i = 0; i < 16; i += 4)
            fprintf(stderr, " %08X", MEM32(va + row * 16 + i));
        fprintf(stderr, "\n");
    }
}

/*
 * A watchdog thread.
 *
 * "It stopped doing anything" is not a crash, so nothing reports it. This
 * prints where the engine is every few seconds: the running lifted function,
 * the instruction, and the top of the shadow call stack. It only READS globals
 * the main thread owns -- it never enters lifted code, because the register
 * file and stack are global and a second thread inside them would corrupt the
 * first.
 */
static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    uint32_t last_pc = 0;
    int same = 0;
    for (;;) {
        Sleep(3000);
        uint32_t pc = g_cur_pc;
        same = (pc == last_pc) ? same + 1 : 0;
        last_pc = pc;
        /*
         * Mission::run is a jump table on this->state, and where it sits is
         * the single most useful number during bring-up: state 10 plays the
         * intro movies, 8 waits for one to finish, 3 is the logistics shell.
         * `movie` is the index it is up to, `mode` the outer game mode at
         * 0x808790, `play` aSystem's "a movie is running" flag.
         */
        uint32_t mission = MEM32(0x008087A8u), sys = MEM32(0x007AAE80u);
        /* BriefingScreen::setUpDeploy walks the roster list whose head lives at
         * [[Logistics + 0x1CC]]. A null head is an empty Ready Area, which is
         * an empty force group, which is a launch button that stays grey. */
        uint32_t logi = MEM32(0x00808634u);
        uint32_t list = logi ? MEM32(logi + 0x1CCu) : 0;
        uint32_t head = list ? MEM32(list) : 0;
        uint32_t list2 = logi ? MEM32(logi + 0x1D0u) : 0;
        uint32_t head2 = list2 ? MEM32(list2) : 0;
        /* setUpDeploy only builds a Ready Area entry for a 'Mech whose
         * [+0xB4] is above -1 and whose [+0x78] is zero, so those two fields
         * decide whether the list draws anything at all. Walk the first few. */
        /* The Ready Area pane as it stands right now: shown? how many
         * children, and are THEY shown? */
        char ready[160] = "";
        if (g_ready_pane) {
            uint32_t sp = g_ready_pane;
            int kids = (int)MEM32(sp + 0x470u), off = 0;
            off += snprintf(ready, sizeof ready, " pane vis=%d kids=%d",
                            (int)MEM32(sp + 0x54u), kids);
            for (int i = 0; i < kids && i < 6 && off < (int)sizeof ready - 32; i++) {
                uint32_t kid = MEM32(sp + 0x74u + 4u * (uint32_t)i);
                off += snprintf(ready + off, sizeof ready - off, " [%d/%d]",
                                kid ? (int)MEM32(kid + 0x54u) : -1,
                                kid ? (int)MEM32(kid + 0x50u) : -1);
            }
            /* And the decisive one: is anything actually PAINTED into the
             * pane's own port? lObject::display composites that port onto the
             * screen but only calls draw() when the object has an animation,
             * so an empty port means the entries never rendered, while a full
             * one means the composite is what is lost. */
            {
                uint32_t port = MEM32(sp + 0x4ACu);
                uint32_t pane = port ? MEM32(port + 0x10u) : 0;
                uint32_t win  = pane ? MEM32(pane) : 0;
                size_t nz = 0, n = 0;
                if (win) {
                    uint32_t w = MEM32(win + 4) + 1, h = MEM32(win + 8) + 1;
                    const uint8_t* px = (const uint8_t*)ADDR(MEM32(win));
                    n = (size_t)w * h;
                    /* setUpDeploy wipes this port to palette index 0x10 before
                     * placing anything, so "non-zero" is every pixel. Count the
                     * ones that are NOT the wipe colour -- those are drawing. */
                    for (size_t k = 0; k < n; k++) if (px[k] != 0x10) nz++;
                }
                snprintf(ready + off, sizeof ready - off, " port %zu/%zu", nz, n);
                /* Once: show the first entry's 52x46 cell. "487 pixels" does
                 * not say WHICH 487 -- outlines, half a glyph, or a picture
                 * that landed one column wide all look the same as a count. */
                if (nz > 0 && win) {
                    static int dumped;
                    uint32_t w = MEM32(win + 4) + 1;
                    const uint8_t* px = (const uint8_t*)ADDR(MEM32(win));
                    if (!dumped) {
                        dumped = 1;
                        fprintf(stderr, "[deploy] first entry cell:\n");
                        for (int yy = 0; yy < 46; yy++) {
                            fprintf(stderr, "      ");
                            for (int xx = 0; xx < 52; xx++)
                                fputc(px[(size_t)(yy + 3) * w + (xx + 4)] == 0x10
                                      ? '.' : '#', stderr);
                            fputc('\n', stderr);
                        }
                    }
                }
            }
        }
        char mechs[128] = "";
        {
            uint32_t m = head;
            int n = 0, off = 0;
            while (m && n < 4 && off < (int)sizeof mechs - 24) {
                off += snprintf(mechs + off, sizeof mechs - off, " [%d,%u]",
                                (int)MEM32(m + 0xB4u), MEM32(m + 0x78u));
                m = MEM32(m + 0x254u);
                n++;
            }
        }
        fprintf(stderr, "[watchdog] func=0x%08X pc=0x%08X depth=%u "
                "peek=%u dispatch=%u wndproc=%u | state=%d movie=%d mode=%d "
                "play=%d roster=%08X/%08X mechs%s%s%s\n",
                g_cur_func, pc, g_callstack_n,
                g_n_peek, g_n_dispatch, g_n_wndproc,
                mission ? (int)MEM32(mission) : -1,
                mission ? (int)MEM32(mission + 0x14) : -1,
                (int)MEM32(0x00808790u),
                sys ? (int)MEM32(sys + 0xAECu) : -1, list, head,
                mechs[0] ? mechs : " none", ready,
                same ? "  (PC unchanged)" : "");
        if (same) {
            uint32_t n = g_callstack_n < CALLSTACK_MAX
                       ? g_callstack_n : CALLSTACK_MAX;
            uint32_t from = n > 8 ? n - 8 : 0;
            for (uint32_t i = from; i < n; i++)
                fprintf(stderr, "[watchdog]   called from 0x%08X\n",
                        g_callstack[i]);
        }
        fflush(stderr);
    }
}

static void start_watchdog(void) {
    if (getenv("MCG_WATCHDOG"))
        CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
}

/* Everything worth printing when something faults, in one place. */
static void report_extra(void) {
    /* Non-zero means the engine was re-entered through the window procedure --
     * so whatever it was drawing may only have been half built. */
    fprintf(stderr, "  window-proc depth: %d\n", g_wndproc_depth);
    dump_around("ecx", g_ecx);
    dump_around("esi", g_esi);
    dump_around("edi", g_edi);
    dump_callstack();
    dump_callsites();
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
 * void __cdecl _cfltcvt(double* value, char* buf, int format, int prec, int caps)
 *
 * printf's float formatter -- __output calls it through [0x7A7150] for every
 * %e/%f/%g, then only touches the result with _forcdecpt and _cropzeros, so
 * what it writes is the final numeric text and the host's printf produces the
 * same thing. Overridden for the same reason __fassign is: it is the head of
 * MSVC6's hand-written 80-bit conversion chain (_cftoe/_cftof/_cftog ->
 * _fptostr), the part of the CRT that leans hardest on x87 precision control
 * and the 12-byte long double, and the lifted version comes back with the
 * caller's ebp gone -- __output then read its own locals through a null frame
 * pointer and faulted on the first menu that formats a number.
 */
static void mcg_cfltcvt(void) {
    uint32_t vp  = MEM32(g_esp + 4);
    uint32_t dst = MEM32(g_esp + 8);
    int fmt  = (int)MEM32(g_esp + 12);
    int prec = (int)MEM32(g_esp + 16);
    int caps = (int)MEM32(g_esp + 20);
    double v = 0.0;
    char spec[16], out[584];           /* __output's own buffer is 0x248 */
    char conv;

    if (vp) memcpy(&v, (const void*)ADDR(vp), sizeof v);
    /* The format char picks the conversion; caps picks its case. */
    if (fmt == 'e' || fmt == 'E')      conv = caps ? 'E' : 'e';
    else if (fmt == 'f' || fmt == 'F') conv = 'f';
    else                               conv = caps ? 'G' : 'g';
    if (prec < 0) prec = 0;
    if (prec > 512) prec = 512;
    snprintf(spec, sizeof spec, "%%.%d%c", prec, conv);
    snprintf(out, sizeof out, spec, v);
    if (dst) memcpy((void*)ADDR(dst), out, strlen(out) + 1);
    g_esp += 4;                        /* __cdecl: the caller cleans up */
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
        /* Keyed on the message pointer as well as the line: plenty of these
         * pass line 0, and deduping on the line alone collapsed all of them
         * into the first one -- which hid every assertion after it. */
        static struct { uint32_t line, msg; } seen[128];
        static uint32_t n;
        uint32_t i = 0;
        for (; i < n; i++)
            if (seen[i].line == line && seen[i].msg == msg) break;
        if (i == n && n < 128) {
            seen[n].line = line;
            seen[n].msg = msg;
            n++;
            fprintf(stderr, "[game:Assert] %s:%u  %s\n",
                    file ? (const char*)ADDR(file) : "?", line,
                    msg ? (const char*)ADDR(msg) : "");
        }
    }
    g_esp += 4;                          /* __cdecl */
}

/*
 * BriefingScreen::setUpDeploy -- report what the Ready Area was given.
 *
 * It builds one entry per roster 'Mech that has a pilot ([+0xB4] above -1) and
 * a zero at [+0x78], counting them into [this+0x4BC] and then sizing an array
 * from that count. An empty Ready Area is either "the roster was empty when
 * this ran" or "it was not empty and the entries did not draw", and those are
 * different bugs -- so print the roster and the count it derived, each time.
 */
void recomp_BriefingScreen__setUpDeploy_006DD410(void);
static void mcg_setUpDeploy(void) {
    uint32_t self = g_ecx;
    uint32_t logi = MEM32(0x00808634u);
    uint32_t list = logi ? MEM32(logi + 0x1CCu) : 0;
    uint32_t m = list ? MEM32(list) : 0;
    int roster = 0;
    for (; m && roster < 64; m = MEM32(m + 0x254u)) roster++;
    recomp_BriefingScreen__setUpDeploy_006DD410();
    fprintf(stderr, "[deploy] setUpDeploy: %d in the roster -> %d entries\n",
            roster, (int)MEM32(self + 0x4BCu));
    {   /* The scroll pane the entries were parented to: does it own them, is
         * it visible, and where does it composite itself? */
        uint32_t sp = MEM32(self + 0x5C0u);
        uint32_t out = sp ? MEM32(sp + 8u) : 0;
        /* Tried and did NOT work, so it is not shipped and should not be
         * retried: forcing [+0x54] to 1 on the pane, and on all three
         * children, leaves the Ready Area just as blank. The flag is one
         * reason lObject::display bails, but it is not the only one -- look
         * further down that function (it also tests [+0x60] and the virtual
         * at vtable+0x10C) before blaming whoever fails to show the pane. */
        fprintf(stderr, "[deploy] pane %08X children=%d visible=%d -> "
                "(%d,%d)-(%d,%d)\n", sp,
                sp ? (int)MEM32(sp + 0x470u) : -1,
                sp ? (int)MEM32(sp + 0x54u) : -1,
                out ? (int)MEM32(out + 4) : -1, out ? (int)MEM32(out + 8) : -1,
                out ? (int)MEM32(out + 12) : -1, out ? (int)MEM32(out + 16) : -1);
    }
}

/* MechBriefBlock::init(LogMech*, lObject* parent, long x, long y) -- the
 * position each Ready Area entry is placed at, inside the scroll pane's port. */
void recomp_MechBriefBlock__init_006DD7C0(void);
static void mcg_MechBriefBlock_init(void) {
    static int shown;
    uint32_t self = g_ecx, mech = MEM32(g_esp + 4), parent = MEM32(g_esp + 8);
    int x = (int)MEM32(g_esp + 12), y = (int)MEM32(g_esp + 16);
    if (shown < 8) {
        shown++;
        /* The entries are placed into the scroll pane's own offscreen port.
         * If that port has no height, every position in it is correct and
         * invisible -- so print the rectangle they are being placed into. */
        uint32_t pp = parent ? MEM32(parent + 0x4ACu) : 0;
        uint32_t pane = pp ? MEM32(pp + 0x10u) : 0;
        uint32_t win = pane ? MEM32(pane) : 0;
        fprintf(stderr, "[deploy] block %08X mech=%08X parent=%08X at (%d,%d) "
                "into port %08X pane (%d,%d)-(%d,%d) win %dx%d\n",
                self, mech, parent, x, y, pp,
                pane ? (int)MEM32(pane + 4) : -1, pane ? (int)MEM32(pane + 8) : -1,
                pane ? (int)MEM32(pane + 12) : -1, pane ? (int)MEM32(pane + 16) : -1,
                win ? (int)MEM32(win + 4) + 1 : -1, win ? (int)MEM32(win + 8) + 1 : -1);
    }
    recomp_MechBriefBlock__init_006DD7C0();
}

/*
 * Who shows what.
 *
 * BriefingScreen::ShowGUIWindow(show) forwards to the Ready Area's scroll pane
 * at [this+0x5C0] unconditionally, so if that pane is hidden then either this
 * never ran or the show did not stick. Both are virtual, so they are reachable
 * through the dispatcher without withholding anything from the lifter.
 */
void recomp_aObject__ShowGUIWindow_0060A780(void);
static void mcg_aObject_ShowGUIWindow(void) {
    static int shown;
    uint32_t self = g_ecx, arg = MEM32(g_esp + 4);
    if (shown < 40) {
        shown++;
        fprintf(stderr, "[show] aObject %08X <- %d\n", self, (int)arg);
    }
    recomp_aObject__ShowGUIWindow_0060A780();
}

void recomp_BriefingScreen__ShowGUIWindow_006DCEF0(void);
static void mcg_BriefingScreen_ShowGUIWindow(void) {
    uint32_t self = g_ecx, arg = MEM32(g_esp + 4), sp = MEM32(self + 0x5C0u);
    g_ready_pane = sp;
    fprintf(stderr, "[show] BriefingScreen %08X <- %d (pane %08X)\n",
            self, (int)arg, sp);
    recomp_BriefingScreen__ShowGUIWindow_006DCEF0();
    fprintf(stderr, "[show] ...pane visible now %d\n",
            sp ? (int)MEM32(sp + 0x54u) : -1);
}

/*
 * lPort::init(char* name) -- does the art actually arrive?
 *
 * MechBriefBlock::drawBackground builds each Ready Area entry out of temporary
 * ports loaded by name and then copies them in. The copies are the part that
 * does not land, and the blitters underneath are all self-tested, so the
 * question is whether the source port has any pixels in it at all.
 */
void recomp_lPort__init_007106D0(void);
static void mcg_lPort_init_named(void) {
    static int shown;
    uint32_t self = g_ecx, name = MEM32(g_esp + 4);
    recomp_lPort__init_007106D0();
    if (shown < 24) {
        uint32_t pane = MEM32(self + 0x10u), win = pane ? MEM32(pane) : 0;
        size_t nz = 0, n = 0;
        if (win) {
            uint32_t w = MEM32(win + 4) + 1, h = MEM32(win + 8) + 1;
            const uint8_t* px = (const uint8_t*)ADDR(MEM32(win));
            n = (size_t)w * h;
            for (size_t k = 0; k < n; k++) if (px[k]) nz++;
            shown++;
            fprintf(stderr, "[art] \"%s\" -> %ux%u, %zu/%zu non-zero\n",
                    name ? (const char*)ADDR(name) : "(null)", w, h, nz, n);
        } else {
            shown++;
            fprintf(stderr, "[art] \"%s\" -> NO WINDOW\n",
                    name ? (const char*)ADDR(name) : "(null)");
        }
    }
}

recomp_func_t recomp_lookup_manual(uint32_t va) {
    switch (va) {
    case 0x007106D0u: return mcg_lPort_init_named;
    case 0x0060A780u: return mcg_aObject_ShowGUIWindow;
    case 0x006DCEF0u: return mcg_BriefingScreen_ShowGUIWindow;
    case 0x006DD410u: return mcg_setUpDeploy;
    case 0x006DD7C0u: return mcg_MechBriefBlock_init;
    case 0x00644250u: return mcg_Assert;       /* Assert    */
    case 0x00760D27u: return mcg_initterm;     /* _initterm */
    case 0x00763EDAu: return mcg_fassign;      /* __fassign  */
    case 0x007641DFu: return mcg_cfltcvt;      /* __cfltcvt  */
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
    /* COM methods: the DirectDraw object graph's vtable slots live in their own
     * range above this one. */
    return ddraw_lookup(va);
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

    /* FitIniFile::textToLong -- the engine's own number parser, and the one
     * every count and coordinate in a logistics screen comes through. It takes
     * a hex path via strstr plus the `repne scasb` strlen idiom, or falls back
     * to atol, so it exercises both of the string primitives fixed above. It is
     * __thiscall with one stack argument; `this` is unused on these paths. */
    {
        const uint32_t TEXTTOLONG = 0x006494B0;
        static const struct { const char* in; uint32_t want; } cases[] = {
            { "123", 123 }, { "0", 0 }, { "4096", 4096 },
            { "0x1E", 0x1E }, { "0x100", 0x100 }, { "0xFF", 0xFF },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            char label[64];
            uint32_t arg = shims_put_str(cases[i].in);
            g_ecx = 0;
            snprintf(label, sizeof label, "textToLong(\"%s\")", cases[i].in);
            expect(label, call_lifted(TEXTTOLONG, 1, &arg), cases[i].want);
        }
    }

    /* printf's float path, end to end: __output -> _cfltcvt -> _forcdecpt /
     * _cropzeros. Every logistics screen formats numbers through it, and it
     * faulted on the first one until _cfltcvt was overridden. Comparing against
     * the host's sprintf checks the override AND the lifted code around it. */
    {
        const uint32_t SPRINTF = 0x0075F973, CFLTCVT_INIT = 0x0075F92B;
        /* The float conversion table starts out pointing at _fptrap, so
         * anything with a %f in it exits with R6002 until _cfltcvt_init has
         * filled it. WinMain gets there through _fpmath; --selftest does not. */
        call_lifted(CFLTCVT_INIT, 0, NULL);
        static const struct { const char* fmt; double v; } cases[] = {
            { "%f",    3.5 },   { "%.2f", 1234.5678 }, { "%f", 0.0 },
            { "%.3e", -42.0 },  { "%g",   0.000125 },  { "%.1f", -0.05 },
        };
        for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
            uint32_t dst = shims_alloc(128), args[4];
            double v = cases[i].v;
            uint32_t bits[2];
            char want[128], label[64];
            memcpy(bits, &v, sizeof bits);
            args[0] = dst;
            args[1] = shims_put_str(cases[i].fmt);
            args[2] = bits[0];
            args[3] = bits[1];
            call_lifted(SPRINTF, 4, args);
            snprintf(want, sizeof want, cases[i].fmt, v);
            snprintf(label, sizeof label, "sprintf(\"%s\", %g)",
                     cases[i].fmt, v);
            {
                const char* got = (const char*)ADDR(dst);
                if (strcmp(got, want)) {
                    fprintf(stderr, "  [FAIL] %-34s got \"%s\"  want \"%s\"\n",
                            label, got, want);
                    g_st_fail++;
                } else {
                    fprintf(stderr, "  [ ok ] %-34s \"%s\"\n", label, got);
                }
            }
        }
    }

    /*
     * VFX_pane_wipe -- the flat-colour fill every widget falls back to when it
     * has no picture, and a member of the same hand-written VFX blit family
     * whose `rep movs` ignored the direction flag. It clips the rectangle
     * against the window, then fills each row as `and ecx,3; rep stosb` for the
     * remainder followed by `shr ecx,2; rep stosd`, which is exactly the shape
     * a lifter gets wrong one byte at a time.
     *
     *   window { uint8_t* buffer; int32_t x_max, y_max; }   (inclusive maxima)
     *   pane   { window* w; int32_t x0, y0, x1, y1; }       (inclusive corners)
     */
    {
        const uint32_t PANE_WIPE = 0x0076F7CC;
        const int W = 16, H = 8;
        uint32_t buf = shims_alloc(W * H);
        uint32_t win = shims_alloc(12), pane = shims_alloc(20), a[2];
        int x0 = 2, y0 = 1, x1 = 9, y1 = 5, bad = 0;
        const uint8_t* px = (const uint8_t*)ADDR(buf);

        memset((void*)ADDR(buf), 0, W * H);
        MEM32(win) = buf; MEM32(win + 4) = W - 1; MEM32(win + 8) = H - 1;
        MEM32(pane) = win;
        MEM32(pane + 4) = (uint32_t)x0; MEM32(pane + 8)  = (uint32_t)y0;
        MEM32(pane + 12) = (uint32_t)x1; MEM32(pane + 16) = (uint32_t)y1;

        a[0] = pane; a[1] = 0xAB;
        expect("VFX_pane_wipe returns 0", call_lifted(PANE_WIPE, 2, a), 0);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int inside = (x >= x0 && x <= x1 && y >= y0 && y <= y1);
                if (px[y * W + x] != (inside ? 0xAB : 0x00)) bad++;
            }
        expect("VFX_pane_wipe 8x5 rect", (uint32_t)bad, 0);
        if (bad) {
            for (int y = 0; y < H; y++) {
                fprintf(stderr, "         ");
                for (int x = 0; x < W; x++)
                    fputc(px[y * W + x] ? '#' : '.', stderr);
                fputc('\n', stderr);
            }
        }
    }

    /*
     * VFX_pane_copy(src, sx, sy, dst, dx, dy, transparent) -- the blit every
     * widget with a picture goes through (lButton::draw -> aPort::copyTo).
     * Its `rep movs` already had to be taught the direction flag once; this
     * pins the result down instead of inferring it from what the screen looks
     * like.
     */
    {
        const uint32_t PANE_COPY = 0x0076F8AB;
        const int SW = 16, SH = 8, DW = 24, DH = 12, DX = 4, DY = 2;
        uint32_t sbuf = shims_alloc(SW * SH), dbuf = shims_alloc(DW * DH);
        uint32_t swin = shims_alloc(12), dwin = shims_alloc(12);
        uint32_t spane = shims_alloc(20), dpane = shims_alloc(20), a[7];
        uint8_t* sp = (uint8_t*)ADDR(sbuf);
        const uint8_t* dp = (const uint8_t*)ADDR(dbuf);
        int bad = 0;

        for (int i = 0; i < SW * SH; i++) sp[i] = (uint8_t)(i | 1);
        memset((void*)ADDR(dbuf), 0, DW * DH);
        MEM32(swin) = sbuf; MEM32(swin + 4) = SW - 1; MEM32(swin + 8) = SH - 1;
        MEM32(dwin) = dbuf; MEM32(dwin + 4) = DW - 1; MEM32(dwin + 8) = DH - 1;
        MEM32(spane) = swin; MEM32(spane + 4) = 0; MEM32(spane + 8) = 0;
        MEM32(spane + 12) = SW - 1; MEM32(spane + 16) = SH - 1;
        MEM32(dpane) = dwin; MEM32(dpane + 4) = 0; MEM32(dpane + 8) = 0;
        MEM32(dpane + 12) = DW - 1; MEM32(dpane + 16) = DH - 1;

        a[0] = spane; a[1] = 0; a[2] = 0;
        a[3] = dpane; a[4] = DX; a[5] = DY; a[6] = 0xFFFFFFFFu;  /* opaque */
        expect("VFX_pane_copy returns 0", call_lifted(PANE_COPY, 7, a), 0);
        for (int y = 0; y < DH; y++)
            for (int x = 0; x < DW; x++) {
                int in = (x >= DX && x < DX + SW && y >= DY && y < DY + SH);
                uint8_t want = in ? sp[(y - DY) * SW + (x - DX)] : 0;
                if (dp[y * DW + x] != want) bad++;
            }
        expect("VFX_pane_copy 16x8 at (4,2)", (uint32_t)bad, 0);
        if (bad) {
            for (int y = 0; y < DH; y++) {
                fprintf(stderr, "         ");
                for (int x = 0; x < DW; x++)
                    fputc(dp[y * DW + x] ? '#' : '.', stderr);
                fputc('\n', stderr);
            }
        }
    }

    /*
     * DrawTransparent(dst_pane, src_window, x, y, w, h) -- what actually puts
     * a widget's port on the screen (lObject::draw -> aPort::copyTo with a
     * non-zero flag). 22 is deliberately not a multiple of 4: these VFX-style
     * blitters copy `w & 3` bytes with rep movsb and the rest with rep movsd,
     * so an aligned width tests only half the loop.
     */
    {
        const uint32_t DRAW_TRANS = 0x006B6F20;
        const int SW = 22, SH = 6, DW = 64, DH = 24, DX = 17, DY = 5;
        uint32_t sbuf = shims_alloc(SW * SH), dbuf = shims_alloc(DW * DH);
        uint32_t swin = shims_alloc(12), dwin = shims_alloc(12);
        uint32_t dpane = shims_alloc(20), a[6];
        uint8_t* sp = (uint8_t*)ADDR(sbuf);
        const uint8_t* dp = (const uint8_t*)ADDR(dbuf);
        int bad = 0;

        /* 0xFF is the transparent index -- the scalar tail reads
         * `cmp al, 0xff; je skip`, and the fast path is MMX doing that same
         * test eight bytes at a time with pcmpeqb/pand/pandn/por, plus
         * psllq/psrlq to realign an unaligned source. A source with no 0xFF
         * in it exercises none of that, which is how this case passed while
         * every transparent picture in the game blitted as nothing. So:
         * sprinkle transparent pixels through the source, and give the
         * destination a marker that has to survive underneath them. */
        for (int i = 0; i < SW * SH; i++)
            sp[i] = (i % 3 == 0) ? 0xFF : (uint8_t)(i | 1);
        memset((void*)ADDR(dbuf), 0x55, DW * DH);
        MEM32(swin) = sbuf; MEM32(swin + 4) = SW - 1; MEM32(swin + 8) = SH - 1;
        MEM32(dwin) = dbuf; MEM32(dwin + 4) = DW - 1; MEM32(dwin + 8) = DH - 1;
        /* The real caller (lObject::display) hands over a pane that already
         * carries the widget's position and passes x=y=0, so the offset has to
         * come out of the pane -- a pane anchored at the origin would leave
         * that half of the arithmetic untested. */
        MEM32(dpane) = dwin; MEM32(dpane + 4) = DX; MEM32(dpane + 8) = DY;
        MEM32(dpane + 12) = DX + SW - 1; MEM32(dpane + 16) = DY + SH - 1;

        a[0] = dpane; a[1] = swin; a[2] = 0; a[3] = 0; a[4] = SW; a[5] = SH;
        call_lifted(DRAW_TRANS, 6, a);
        for (int y = 0; y < DH; y++)
            for (int x = 0; x < DW; x++) {
                int in = (x >= DX && x < DX + SW && y >= DY && y < DY + SH);
                uint8_t src = in ? sp[(y - DY) * SW + (x - DX)] : 0xFF;
                /* 0xFF leaves whatever was already there. */
                uint8_t want = (src == 0xFF) ? 0x55 : src;
                if (dp[y * DW + x] != want) bad++;
            }
        expect("DrawTransparent 22x6, transparent", (uint32_t)bad, 0);

        /*
         * Again at the shape the game actually uses. DrawTransparent's fast
         * path reads eight bytes at a time and only runs its psllq/psrlq
         * realignment when the source is NOT 8-aligned -- so a tidy little
         * test buffer can skip that half of the routine entirely. The Ready
         * Area's art is 194 and 207 bytes wide, which guarantees a different
         * misalignment on every row.
         */
        {
            const int W = 194, H = 12, DW2 = 256, DH2 = 32, X2 = 5, Y2 = 3;
            uint32_t sb = shims_alloc(W * H + 8) + 3;      /* deliberately odd */
            uint32_t db = shims_alloc(DW2 * DH2);
            uint32_t sw = shims_alloc(12), dw = shims_alloc(12);
            uint32_t dpn = shims_alloc(20), b[6];
            uint8_t* s2 = (uint8_t*)ADDR(sb);
            const uint8_t* d2 = (const uint8_t*)ADDR(db);
            int bad2 = 0;

            for (int i = 0; i < W * H; i++)
                s2[i] = (i % 5 == 0) ? 0xFF : (uint8_t)(i | 1);
            memset((void*)ADDR(db), 0x55, DW2 * DH2);
            MEM32(sw) = sb; MEM32(sw + 4) = W - 1; MEM32(sw + 8) = H - 1;
            MEM32(dw) = db; MEM32(dw + 4) = DW2 - 1; MEM32(dw + 8) = DH2 - 1;
            MEM32(dpn) = dw; MEM32(dpn + 4) = X2; MEM32(dpn + 8) = Y2;
            MEM32(dpn + 12) = X2 + W - 1; MEM32(dpn + 16) = Y2 + H - 1;

            b[0] = dpn; b[1] = sw; b[2] = 0; b[3] = 0; b[4] = W; b[5] = H;
            call_lifted(DRAW_TRANS, 6, b);
            for (int y = 0; y < DH2; y++)
                for (int x = 0; x < DW2; x++) {
                    int in = (x >= X2 && x < X2 + W && y >= Y2 && y < Y2 + H);
                    uint8_t src = in ? s2[(y - Y2) * W + (x - X2)] : 0xFF;
                    uint8_t want = (src == 0xFF) ? 0x55 : src;
                    if (d2[y * DW2 + x] != want) bad2++;
                }
            expect("DrawTransparent 194x12, unaligned", (uint32_t)bad2, 0);
            if (bad2) {
                fprintf(stderr, "         first row, want vs got:\n");
                for (int k = 0; k < 40; k++) {
                    uint8_t src = s2[k];
                    fprintf(stderr, "%s%02X/%02X", k ? " " : "         ",
                            src == 0xFF ? 0x55 : src, d2[Y2 * DW2 + X2 + k]);
                }
                fputc('\n', stderr);
            }
        }
        if (bad) {
            for (int y = 0; y < DH; y++) {
                fprintf(stderr, "         ");
                for (int x = 0; x < DW; x++)
                    fputc(dp[y * DW + x] ? '#' : '.', stderr);
                fputc('\n', stderr);
            }
        }
    }

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

    watch_init();
    start_watchdog();
    fprintf(stderr, "[runtime] entering at 0x%08X\n", MCG_ENTRY);
    PUSH32(g_esp, RECOMP_RETADDR);
    entry();

    fprintf(stderr, "[runtime] entry returned; %u indirect calls\n", g_icall_count);
    return 0;
}

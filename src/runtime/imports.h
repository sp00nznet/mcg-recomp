/*
 * MechCommander Gold recompilation - import bridge helpers.
 *
 * Shims run inside the recomp register/stack model, not the host's. When lifted
 * code calls an import, RECOMP_ICALL has already pushed a dummy return address,
 * so from ESP the simulated stack reads: [ret][arg0][arg1]... one 32-bit slot
 * each.
 *
 *   ARG(n)     read argument n (0-based)
 *   ARGP(n,T)  read argument n as a host pointer of type T* (maps VA -> host)
 *   RET(v)     set the return value (EAX)
 *   STDRET(k)  stdcall epilogue: pop the dummy ret addr plus k argument slots
 *
 * STDRET's count has to be exactly right. Pop too few or too many and the
 * simulated stack silently desynchronises, so every value read after that call
 * is wrong and the symptom appears nowhere near the cause. gen_imports.py
 * derives every count from three independent sources and verifies the bodies in
 * shims_impl.c against them on each run -- never hand-tune one to "make it work".
 */
#ifndef MCG_IMPORTS_H
#define MCG_IMPORTS_H

#include <stdio.h>
#include "recomp_types.h"

#define ARG(n)      MEM32(g_esp + 4 + (n)*4)
#define ARGP(n, T)  ((T*)(void*)(uintptr_t)ADDR(ARG(n)))
#define RET(v)      do { g_eax = (uint32_t)(v); } while (0)
#define STDRET(k)   do { g_esp += 4 + (uint32_t)(k)*4; } while (0)

/* Log an unimplemented import once, by name. */
#define IMPORT_STUB(nm) do {                          \
    static int _warned = 0;                           \
    if (!_warned) {                                   \
        fprintf(stderr, "[import-stub] %s\n", (nm));  \
        _warned = 1;                                  \
    }                                                 \
} while (0)

/* Exports the game resolves at runtime via LoadLibrary/GetProcAddress. These
 * are not in the import table, so they get no IAT slot -- GetProcAddress hands
 * out a synthetic VA instead and the runtime resolves that back to the shim. */
typedef struct {
    const char* module;
    const char* proc;
    recomp_func_t func;
} mcg_dyn_export_t;

extern const mcg_dyn_export_t mcg_dyn_exports[];
extern const uint32_t mcg_dyn_export_count;

/* VA for (module, proc), or 0 if we do not implement it. */
uint32_t mcg_dyn_resolve(const char* module, const char* proc);

/* Generated bridge tables (imports_gen.c). */
extern const recomp_dispatch_entry_t mcg_import_bridges[];
extern const uint32_t mcg_import_bridge_count;
extern const uint32_t mcg_iat_slots[];
extern const uint32_t mcg_iat_slot_count;

/* Point every IAT slot at itself, so that a lifted `call dword ptr [slot]`
 * -- which lifts to RECOMP_ICALL(MEM32(slot)) -- yields the slot's own VA and
 * recomp_lookup_import can find its bridge. The file's original slot contents
 * are RVAs into the hint/name table and resolve to nothing. */
void mcg_install_iat(void);

/* Simulated-memory helpers shared between the runtime and the shims. */
int      shims_init(uint32_t base);
uint32_t shims_arena_base(void);
uint32_t shims_arena_end(void);
uint32_t shims_make_tib(uint32_t stack_base, uint32_t stack_top);
void     shims_set_program(const char* path);
void     shims_report(void);
uint32_t shims_put_str(const char* s);
uint32_t shims_alloc(uint32_t n);
void*    shims_handle(uint32_t v);   /* 32-bit handle -> host pointer */

/* The DirectDraw mock (ddraw.c): one COM object graph built in simulated
 * memory, whose vtable slots are synthetic VAs the dispatcher resolves. */
uint32_t      ddraw_create(void);
uint32_t      dsound_create(void);
uint32_t      dplay_lobby_create(void);
uint32_t      dplay_create(void);
recomp_func_t ddraw_lookup(uint32_t va);   /* resolves every COM method VA */

/* Region bounds, so VirtualQuery can answer without duplicating the layout. */
uint32_t recomp_image_base(void);
uint32_t recomp_image_end(void);
uint32_t recomp_stack_base(void);
uint32_t recomp_stack_end(void);

#endif /* MCG_IMPORTS_H */

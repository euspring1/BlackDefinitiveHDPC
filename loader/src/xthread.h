// Per-thread Xbox state.
//
// Xbox code addresses its KPCR through fs. Windows keeps its TEB in fs and needs it, so instead of
// swapping fs, the loader rewrites the few fs:[disp32] instructions that read Xbox-only KPCR fields so
// they read Windows TLS slots (TEB TlsSlots at fs:[0xE10 + 4*i]) instead. fs:[0] (SEH chain) has the same
// meaning on both and is left alone. Fields remapped (from the disassembly of this XBE, see NOTES.md):
//   fs:[0x04]  NtTib.StackBase, used by XAPI as the TLS array  -> slot: TlsData + TlsDataSize
//   fs:[0x20]  KPCR.Prcb                                       -> slot: fake KPRCB
//   fs:[0x24]  KPCR.Irql (byte)                                -> slot: current IRQL
//   fs:[0x28]  KPRCB.CurrentThread                             -> slot: fake KTHREAD/ETHREAD
//   fs:[0x58]  KPRCB.DpcRoutineActive                          -> slot: 1 while a DPC runs
#pragma once

#include "common.h"

#include <vector>

class Xbe;

enum XSlot { XSLOT_TLS_ARRAY, XSLOT_PRCB, XSLOT_IRQL, XSLOT_THREAD, XSLOT_DPC_ACTIVE, XSLOT_COUNT };

// Xbox structure offsets read by game/XAPI code.
constexpr uint32_t KTHREAD_TLS_DATA = 0x28;        // KTHREAD.TlsData
constexpr uint32_t ETHREAD_UNIQUE_THREAD = 0x12C;  // ETHREAD.UniqueThread (XAPI GetCurrentThreadId)
constexpr uint32_t ETHREAD_START_ADDRESS = 0x130;  // ETHREAD.StartAddress
constexpr uint32_t KPRCB_DEBUG_MONITOR = 0x250;    // checked for a debugger; 0 = none

struct XThread {
    // Fake ETHREAD (KTHREAD is its first member). Must stay first: the kernel hands out &ethread as
    // the KTHREAD pointer and maps it back with xthread_from_kthread.
    alignas(16) uint8_t ethread[0x200];
    alignas(16) uint8_t prcb[0x300]; // fake KPRCB, +0 CurrentThread
    uint8_t *tls_data = nullptr;
    uint32_t tls_size = 0;
    HANDLE host_handle = nullptr; // real handle owned by the loader (duplicated)
    DWORD host_tid = 0;
    uint8_t irql = 0;
    bool holds_dispatch_lock = false;
    bool cdecl_entry = false; // start_routine is the XBE entry point (cdecl, no arguments)
    int base_priority = 0; // Xbox priority increment
    uint32_t start_routine = 0, start_context = 0, system_routine = 0;
    char name[32] = {};
};

bool xthread_init_slots();                 // allocate the TLS slots; call once before patching
uint32_t xthread_slot_fs_offset(XSlot s);  // fs displacement of a slot
XThread *xthread_create_context(uint32_t tls_size);
void xthread_bind(XThread *t);             // make t the current thread's Xbox context
XThread *xthread_current();                // current context, creating one for stray threads
XThread *xthread_current_or_null();
XThread *xthread_from_kthread(const void *kthread);
XThread *xthread_from_handle(HANDLE h);
void xthread_register(XThread *t);
std::vector<XThread *> xthread_all();
void xthread_set_dpc_active(bool active);
inline void *xthread_kthread(XThread *t) { return t->ethread; }

// IRQL emulation. Raising to DISPATCH_LEVEL (2) or above takes a global lock, which gives the
// mutual exclusion that code written for the single-CPU Xbox gets from raising IRQL.
uint8_t xthread_raise_irql(uint8_t new_irql);
void xthread_lower_irql(uint8_t new_irql);

// Rewrite fs:[disp32] accesses in the mapped image's code sections. Returns the number patched.
int patch_fs_accesses(const Xbe &xbe);

#include "xthread.h"

#include "xbe.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace {

DWORD g_slot[XSLOT_COUNT];
// fs displacement each Xbox KPCR field is remapped to, indexed like the table in patch_fs_accesses.
struct Remap {
    uint32_t xbox_disp;
    XSlot slot;
    const char *what;
};
const Remap kRemaps[] = {
    {0x04, XSLOT_TLS_ARRAY, "NtTib.StackBase (TLS array)"},
    {0x20, XSLOT_PRCB, "KPCR.Prcb"},
    {0x24, XSLOT_IRQL, "KPCR.Irql"},
    {0x28, XSLOT_THREAD, "KPRCB.CurrentThread"},
    {0x58, XSLOT_DPC_ACTIVE, "KPRCB.DpcRoutineActive"},
};

thread_local XThread *t_current;
std::mutex g_reg_mutex;
std::map<DWORD, XThread *> g_by_tid;
std::set<const void *> g_valid;
CRITICAL_SECTION g_dispatch_lock;

} // namespace

bool xthread_init_slots()
{
    InitializeCriticalSection(&g_dispatch_lock);
    for (int i = 0; i < XSLOT_COUNT; i++) {
        g_slot[i] = TlsAlloc();
        // Only the first 64 slots live inside the TEB at a fixed fs offset.
        if (g_slot[i] == TLS_OUT_OF_INDEXES || g_slot[i] >= 64) {
            logf("TlsAlloc gave slot %lu; need one below 64 for fs-relative access", g_slot[i]);
            return false;
        }
    }
    logf("xbox fs slots: tls_array=fs:[0x%x] prcb=fs:[0x%x] irql=fs:[0x%x] thread=fs:[0x%x] dpc=fs:[0x%x]",
         xthread_slot_fs_offset(XSLOT_TLS_ARRAY), xthread_slot_fs_offset(XSLOT_PRCB), xthread_slot_fs_offset(XSLOT_IRQL),
         xthread_slot_fs_offset(XSLOT_THREAD), xthread_slot_fs_offset(XSLOT_DPC_ACTIVE));
    return true;
}

uint32_t xthread_slot_fs_offset(XSlot s) { return 0xE10 + 4 * g_slot[s]; }

XThread *xthread_create_context(uint32_t tls_size)
{
    XThread *t = new XThread();
    memset(t->ethread, 0, sizeof t->ethread);
    memset(t->prcb, 0, sizeof t->prcb);
    t->ethread[0] = 6;                       // DISPATCHER_HEADER.Type = ThreadObject
    t->ethread[2] = (uint8_t)(0x110 / 4);    // Size of KTHREAD in dwords
    if (tls_size) {
        t->tls_size = tls_size;
        t->tls_data = (uint8_t *)_aligned_malloc(tls_size + 16, 16);
        memset(t->tls_data, 0, tls_size + 16);
    }
    *(uint32_t *)(t->ethread + KTHREAD_TLS_DATA) = (uint32_t)(uintptr_t)t->tls_data;
    *(uint32_t *)(t->prcb + 0) = (uint32_t)(uintptr_t)t->ethread;
    std::lock_guard<std::mutex> g(g_reg_mutex);
    g_valid.insert(t->ethread);
    return t;
}

void xthread_register(XThread *t)
{
    std::lock_guard<std::mutex> g(g_reg_mutex);
    g_by_tid[t->host_tid] = t;
    g_valid.insert(t->ethread);
}

void xthread_bind(XThread *t)
{
    t->host_tid = GetCurrentThreadId();
    *(uint32_t *)(t->ethread + ETHREAD_UNIQUE_THREAD) = t->host_tid;
    *(uint32_t *)(t->ethread + ETHREAD_START_ADDRESS) = t->start_routine;
    TlsSetValue(g_slot[XSLOT_TLS_ARRAY], t->tls_data ? t->tls_data + t->tls_size : nullptr);
    TlsSetValue(g_slot[XSLOT_PRCB], t->prcb);
    TlsSetValue(g_slot[XSLOT_IRQL], (void *)(uintptr_t)t->irql);
    TlsSetValue(g_slot[XSLOT_THREAD], t->ethread);
    TlsSetValue(g_slot[XSLOT_DPC_ACTIVE], nullptr);
    t_current = t;
    xthread_register(t);
    if (g_game_cpu) SetThreadAffinityMask(GetCurrentThread(), g_game_cpu);
    if (g_cfg.watch_count) {
        // Hardware write breakpoints (DR0-3, 4-byte, write-only) for --watch.
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(GetCurrentThread(), &c);
        DWORD *dr[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
        c.Dr7 = 0;
        for (int i = 0; i < g_cfg.watch_count; i++) {
            *dr[i] = g_cfg.watch[i];
            c.Dr7 |= (1u << (i * 2)) | (0x1u << (16 + i * 4)) | (0x3u << (18 + i * 4));
        }
        if (!SetThreadContext(GetCurrentThread(), &c)) logf("watch: SetThreadContext failed (%lu)", GetLastError());
    }
}

XThread *xthread_current_or_null() { return t_current; }

std::vector<XThread *> xthread_all()
{
    std::lock_guard<std::mutex> g(g_reg_mutex);
    std::vector<XThread *> v;
    for (auto &kv : g_by_tid) v.push_back(kv.second);
    return v;
}

XThread *xthread_current()
{
    if (t_current) return t_current;
    // A thread the loader did not create is calling Xbox code or the kernel. Give it a context.
    XThread *t = xthread_create_context(0);
    snprintf(t->name, sizeof t->name, "stray-%lu", GetCurrentThreadId());
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host_handle, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    xthread_bind(t);
    logf("warning: host thread %lu entered Xbox code without a context; created one", GetCurrentThreadId());
    return t;
}

XThread *xthread_from_kthread(const void *kthread)
{
    std::lock_guard<std::mutex> g(g_reg_mutex);
    if (g_valid.count(kthread)) return (XThread *)kthread; // ethread is the first member
    return nullptr;
}

XThread *xthread_from_handle(HANDLE h)
{
    if (h == (HANDLE)(LONG_PTR)-2) return xthread_current();
    DWORD tid = GetThreadId(h);
    if (!tid) return nullptr;
    std::lock_guard<std::mutex> g(g_reg_mutex);
    auto it = g_by_tid.find(tid);
    return it == g_by_tid.end() ? nullptr : it->second;
}

void xthread_set_dpc_active(bool active) { TlsSetValue(g_slot[XSLOT_DPC_ACTIVE], (void *)(uintptr_t)(active ? 1 : 0)); }

uint8_t xthread_raise_irql(uint8_t new_irql)
{
    XThread *t = xthread_current();
    uint8_t old = t->irql;
    if (new_irql >= 2 && !t->holds_dispatch_lock) {
        EnterCriticalSection(&g_dispatch_lock);
        t->holds_dispatch_lock = true;
    }
    t->irql = new_irql;
    TlsSetValue(g_slot[XSLOT_IRQL], (void *)(uintptr_t)new_irql);
    return old;
}

void xthread_lower_irql(uint8_t new_irql)
{
    XThread *t = xthread_current();
    t->irql = new_irql;
    TlsSetValue(g_slot[XSLOT_IRQL], (void *)(uintptr_t)new_irql);
    if (new_irql < 2 && t->holds_dispatch_lock) {
        t->holds_dispatch_lock = false;
        LeaveCriticalSection(&g_dispatch_lock);
    }
}

// --- fs patching ---------------------------------------------------------------------------------

namespace {

// Opcodes that take a ModRM operand and appear with an fs: prefix in XDK code (loads, stores, ALU).
bool modrm_opcode(uint8_t op)
{
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x84: case 0x85:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x80: case 0x81: case 0x83: case 0xC6:
    case 0xC7: case 0xF6: case 0xF7: case 0xFF: case 0x8F:
        return true;
    }
    return false;
}

bool is_code_section(const XbeSection &s)
{
    if (!(s.flags & XBE_SEC_EXECUTABLE)) return false;
    if (s.name == ".rdata" || s.name == ".data" || s.name == ".XTLID" || s.name.rfind("$$", 0) == 0) return false;
    return true;
}

} // namespace

int patch_fs_accesses(const Xbe &xbe)
{
    int patched = 0, other = 0;
    for (const auto &s : xbe.sections) {
        if (!is_code_section(s)) continue;
        uint8_t *p = (uint8_t *)(uintptr_t)s.vaddr;
        uint32_t n = s.raw_size;
        for (uint32_t i = 0; i + 7 <= n; i++) {
            if (p[i] != 0x64) continue;
            uint32_t disp_off = 0;
            uint8_t op = p[i + 1];
            if (op == 0xA1 || op == 0xA3 || op == 0xA0 || op == 0xA2) {
                disp_off = i + 2;
            } else if (op == 0x0F && (p[i + 2] == 0xB6 || p[i + 2] == 0xB7 || p[i + 2] == 0xBE || p[i + 2] == 0xBF) &&
                       (p[i + 3] & 0xC7) == 0x05) {
                disp_off = i + 4;
            } else if (modrm_opcode(op) && (p[i + 2] & 0xC7) == 0x05) {
                disp_off = i + 3;
            } else {
                continue;
            }
            if (disp_off + 4 > n) continue;
            uint32_t disp;
            memcpy(&disp, p + disp_off, 4);
            if (disp == 0) continue; // ExceptionList: same in the Windows TEB
            const Remap *r = nullptr;
            for (const auto &m : kRemaps)
                if (m.xbox_disp == disp) r = &m;
            if (!r) {
                if (disp < 0x100) {
                    logf("fs patch: unhandled fs:[0x%x] at 0x%08x (%s), left as is", disp, s.vaddr + i, s.name.c_str());
                    other++;
                }
                continue;
            }
            uint32_t nd = xthread_slot_fs_offset(r->slot);
            memcpy(p + disp_off, &nd, 4);
            if (g_cfg.trace) logf("fs patch: 0x%08x fs:[0x%02x] -> fs:[0x%x]  %s", s.vaddr + i, disp, nd, r->what);
            patched++;
            i = disp_off + 3;
        }
    }
    logf("fs patch: %d instructions remapped to TLS slots, %d other small fs offsets left alone", patched, other);
    return patched;
}

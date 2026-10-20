// The game's CPU for the game alone. The game threads run on one logical CPU (g_game_cpu, main.cpp), as on the Xbox,
// and the port keeps its own helper threads off it. Threads that Direct3D, the graphics driver and Windows start
// in the process take the process's mask, which holds that CPU too: the NVIDIA driver's worker thread (about 70%
// of a CPU in busy scenes) was sharing it with the render thread, and the performance recording counted 4-8 ms a
// frame of "other game threads" on the game's CPU. Once a second every thread whose mask holds the game CPU and
// more is moved to the others; the game threads (exactly the game CPU) are left alone.
#include "cpu_guard.h"

#include "common.h"

#include <tlhelp32.h>

#include <set>

namespace {

uint32_t thread_affinity(HANDLE h)
{
    struct Tbi {
        LONG exit_status;
        void *teb;
        void *pid, *tid;
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } tbi = {};
    using Query = LONG(NTAPI *)(HANDLE, int, void *, ULONG, ULONG *);
    static auto query = (Query)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    return query && query(h, 0 /* ThreadBasicInformation */, &tbi, sizeof tbi, nullptr) >= 0 ? (uint32_t)tbi.affinity : 0;
}

void sweep(std::set<DWORD> &moved)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = {sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || moved.count(te.th32ThreadID)) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION |
                                  THREAD_SET_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        uint32_t a = thread_affinity(h);
        if (a && (a & g_game_cpu) && (a & ~(uint32_t)g_game_cpu)) {
            if (SetThreadAffinityMask(h, a & ~(uint32_t)g_game_cpu)) {
                moved.insert(te.th32ThreadID);
                logf("cpu: thread %lu moved off the game CPU (0x%x -> 0x%x)", te.th32ThreadID, a, a & ~(uint32_t)g_game_cpu);
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
}

DWORD WINAPI guard_thread(void *)
{
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    SetThreadAffinityMask(GetCurrentThread(), proc & ~g_game_cpu);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::set<DWORD> moved;
    for (;;) {
        sweep(moved);
        Sleep(1000);
    }
}

} // namespace

void cpu_guard_start()
{
    if (!g_game_cpu) return; // --multicore: no game CPU to keep
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    if (!(proc & ~g_game_cpu)) return; // one CPU only
    if (HANDLE h = CreateThread(nullptr, 0, guard_thread, nullptr, 0, nullptr)) CloseHandle(h);
}

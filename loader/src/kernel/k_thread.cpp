// Threads. Every Xbox thread is a real Windows thread with an XThread context bound to it.
#include "kernel/kexports.h"
#include "kernel/nt.h"
#include "xthread.h"

namespace {

constexpr uint32_t MIN_HOST_STACK = 1u << 20; // Xbox stacks are small; host code called from them needs more

DWORD WINAPI xbox_thread_main(void *param)
{
    XThread *t = (XThread *)param;
    xthread_bind(t);
    logf("thread '%s' start: routine 0x%08x context 0x%08x system 0x%08x tls %u", t->name, t->start_routine, t->start_context,
         t->system_routine, t->tls_size);
    if (t->system_routine)
        ((XPKSYSTEM_ROUTINE)(uintptr_t)t->system_routine)((XPKSTART_ROUTINE)(uintptr_t)t->start_routine,
                                                          (void *)(uintptr_t)t->start_context);
    else if (t->cdecl_entry) // boot thread: the XBE entry point is cdecl with no arguments
        ((void(__cdecl *)())(uintptr_t)t->start_routine)();
    else
        ((XPKSTART_ROUTINE)(uintptr_t)t->start_routine)((void *)(uintptr_t)t->start_context);
    logf("thread '%s' returned", t->name);
    return 0;
}

int host_priority(int32_t xbox_increment)
{
    if (xbox_increment >= 15) return THREAD_PRIORITY_TIME_CRITICAL;
    if (xbox_increment <= -15) return THREAD_PRIORITY_IDLE;
    if (xbox_increment > 2) return THREAD_PRIORITY_HIGHEST;
    if (xbox_increment < -2) return THREAD_PRIORITY_LOWEST;
    return xbox_increment; // -2..2 map 1:1 (THREAD_PRIORITY_LOWEST..HIGHEST)
}

std::atomic<int> g_thread_counter;

} // namespace

NTSTATUS kthread_create(HANDLE *out_handle, DWORD *out_tid, uint32_t stack_size, uint32_t tls_size, uint32_t start_routine,
                        uint32_t start_context, uint32_t system_routine, bool suspended, const char *name, bool cdecl_entry)
{
    XThread *t = xthread_create_context(tls_size);
    t->cdecl_entry = cdecl_entry;
    t->start_routine = start_routine;
    t->start_context = start_context;
    t->system_routine = system_routine;
    if (name)
        snprintf(t->name, sizeof t->name, "%s", name);
    else
        snprintf(t->name, sizeof t->name, "xbox-%d", ++g_thread_counter);
    DWORD tid = 0;
    SIZE_T stack = stack_size < MIN_HOST_STACK ? MIN_HOST_STACK : stack_size;
    HANDLE h = CreateThread(nullptr, stack, xbox_thread_main, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h) {
        logf("[k] CreateThread for '%s' (stack 0x%lx) failed: error %lu", t->name, (unsigned long)stack, GetLastError());
        return XSTATUS_NO_MEMORY;
    }
    t->host_handle = h;
    t->host_tid = tid;
    xthread_register(t);
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    if (out_handle) *out_handle = dup;
    else CloseHandle(dup);
    if (out_tid) *out_tid = tid;
    if (!suspended) ResumeThread(h);
    return XSTATUS_SUCCESS;
}

namespace xk {

NTSTATUS __stdcall PsCreateSystemThreadEx(HANDLE *ThreadHandle, uint32_t ThreadExtensionSize, uint32_t KernelStackSize,
                                          uint32_t TlsDataSize, uint32_t *ThreadId, void *StartRoutine, void *StartContext,
                                          BOOLEAN CreateSuspended, BOOLEAN DebuggerThread, void *SystemRoutine)
{
    DWORD tid = 0;
    NTSTATUS s = kthread_create(ThreadHandle, &tid, KernelStackSize, TlsDataSize, (uint32_t)(uintptr_t)StartRoutine,
                                (uint32_t)(uintptr_t)StartContext, (uint32_t)(uintptr_t)SystemRoutine, CreateSuspended != 0, nullptr);
    if (ThreadId) *ThreadId = tid;
    logf("[k] PsCreateSystemThreadEx(ext 0x%x, stack 0x%x, tls 0x%x, start %s, ctx %p, suspended %u, system %s) -> 0x%08x tid %lu "
         "handle %p",
         ThreadExtensionSize, KernelStackSize, TlsDataSize, sym_name((uint32_t)(uintptr_t)StartRoutine).c_str(), StartContext,
         CreateSuspended, sym_name((uint32_t)(uintptr_t)SystemRoutine).c_str(), s, tid, ThreadHandle ? *ThreadHandle : nullptr);
    return s;
}

void __stdcall PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    XThread *t = xthread_current();
    logf("[k] PsTerminateSystemThread(0x%08x) thread '%s'", ExitStatus, t->name);
    if (t->holds_dispatch_lock) xthread_lower_irql(0);
    ExitThread((DWORD)ExitStatus);
}

int32_t __stdcall KeQueryBasePriorityThread(void *Thread)
{
    KTRACE("%p", Thread);
    XThread *t = xthread_from_kthread(Thread);
    return t ? t->base_priority : 0;
}

int32_t __stdcall KeSetBasePriorityThread(void *Thread, int32_t Increment)
{
    KTRACE("%p, %d", Thread, Increment);
    XThread *t = xthread_from_kthread(Thread);
    if (!t) {
        logf("[k] KeSetBasePriorityThread: unknown thread object %p", Thread);
        return 0;
    }
    int32_t old = t->base_priority;
    t->base_priority = Increment;
    if (t->host_handle) SetThreadPriority(t->host_handle, host_priority(Increment));
    return old;
}

NTSTATUS __stdcall KeDelayExecutionThread(uint8_t WaitMode, BOOLEAN Alertable, LARGE_INTEGER *Interval)
{
    KTRACE("%u, %u, %lld", WaitMode, Alertable, Interval ? Interval->QuadPart : 0LL);
    // Above 60 fps, short relative delays (the game's Sleep(1) every third frame, FUN_001F9130 at 0x1F91AC, which
    // lets its other threads run) yield the CPU until the time is up instead of sleeping: a host sleep wakes on the
    // system timer, which on some PCs is 7-20 ms late even for 1 ms (measured: Sleep(1) 21 ms, a high-resolution
    // waitable timer 8 ms), and that capped 240 fps near 130. SwitchToThread runs the game's other threads first.
    // The delay is there for those threads only: once none of them is waiting to run (SwitchToThread finds nothing),
    // the rest of the time would be spent spinning on the game's CPU, about 0.3 ms a frame, so it ends there.
    if (g_cfg.fps > 60 && Interval && Interval->QuadPart < 0 && Interval->QuadPart >= -50000) { // up to 5 ms
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const int64_t ticks = -Interval->QuadPart * f.QuadPart / 10000000;
        do {
            if (!SwitchToThread()) break;
            QueryPerformanceCounter(&t);
        } while (t.QuadPart - t0.QuadPart < ticks);
        return 0;
    }
    return host::NtDelayExecution(Alertable, Interval);
}

void __stdcall KeStallExecutionProcessor(uint32_t MicroSeconds)
{
    KTRACE("%u", MicroSeconds);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    int64_t ticks = (int64_t)MicroSeconds * f.QuadPart / 1000000;
    do {
        YieldProcessor();
        QueryPerformanceCounter(&b);
    } while (b.QuadPart - a.QuadPart < ticks);
}

NTSTATUS __stdcall NtResumeThread(HANDLE ThreadHandle, uint32_t *PreviousSuspendCount)
{
    KTRACE("%p", ThreadHandle);
    ULONG prev = 0;
    NTSTATUS s = host::NtResumeThread(ThreadHandle, &prev);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    return s;
}

NTSTATUS __stdcall NtSuspendThread(HANDLE ThreadHandle, uint32_t *PreviousSuspendCount)
{
    KTRACE("%p", ThreadHandle);
    ULONG prev = 0;
    NTSTATUS s = host::NtSuspendThread(ThreadHandle, &prev);
    if (PreviousSuspendCount) *PreviousSuspendCount = prev;
    return s;
}

NTSTATUS __stdcall NtYieldExecution()
{
    KTRACE("");
    return host::NtYieldExecution();
}

NTSTATUS __stdcall ObReferenceObjectByHandle(HANDLE Handle, void *ObjectType, void **ReturnedObject)
{
    KTRACE("%p, %p, %p", Handle, ObjectType, ReturnedObject);
    if (ObjectType == PsThreadObjectType || ObjectType == nullptr) {
        if (XThread *t = xthread_from_handle(Handle)) {
            *ReturnedObject = xthread_kthread(t);
            return XSTATUS_SUCCESS;
        }
        if (ObjectType) {
            logf("[k] ObReferenceObjectByHandle(%p): not a known thread handle", Handle);
            return XSTATUS_INVALID_HANDLE;
        }
    }
    logf("[k] ObReferenceObjectByHandle(%p, type %p): object type not supported yet", Handle, ObjectType);
    *ReturnedObject = nullptr;
    return XSTATUS_OBJECT_TYPE_MISMATCH;
}

void __fastcall ObfDereferenceObject(void *Object) { KTRACE("%p", Object); }

} // namespace xk

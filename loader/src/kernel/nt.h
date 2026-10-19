// Host (Windows) ntdll entry points used to implement Xbox kernel calls whose NT semantics match.
// Declared here because the SDK's winternl.h covers only a few of them.
#pragma once

#include "common.h"

namespace host {

struct UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    UNICODE_STRING *ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

struct TIME_FIELDS {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
};

constexpr ULONG OBJ_CASE_INSENSITIVE = 0x40;

inline void init_oa(OBJECT_ATTRIBUTES &oa, UNICODE_STRING *name, HANDLE root = nullptr)
{
    oa.Length = sizeof oa;
    oa.RootDirectory = root;
    oa.ObjectName = name;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    oa.SecurityDescriptor = nullptr;
    oa.SecurityQualityOfService = nullptr;
}

extern "C" {
__declspec(dllimport) NTSTATUS NTAPI NtClose(HANDLE);
__declspec(dllimport) NTSTATUS NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, PLARGE_INTEGER,
                                                  ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtOpenFile(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES *, IO_STATUS_BLOCK *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtReadFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, PLARGE_INTEGER,
                                                PULONG);
__declspec(dllimport) NTSTATUS NTAPI NtWriteFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, PLARGE_INTEGER,
                                                 PULONG);
__declspec(dllimport) NTSTATUS NTAPI NtQueryInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtSetInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, PVOID, ULONG, ULONG,
                                                          BOOLEAN, UNICODE_STRING *, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *, PVOID);
__declspec(dllimport) NTSTATUS NTAPI NtFlushBuffersFile(HANDLE, IO_STATUS_BLOCK *);
__declspec(dllimport) NTSTATUS NTAPI NtFsControlFile(HANDLE, HANDLE, PVOID, PVOID, IO_STATUS_BLOCK *, ULONG, PVOID, ULONG, PVOID,
                                                     ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, PVOID *, ULONG_PTR, PSIZE_T, ULONG, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG);
__declspec(dllimport) NTSTATUS NTAPI NtCreateEvent(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES *, ULONG, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtSetEvent(HANDLE, PLONG);
__declspec(dllimport) NTSTATUS NTAPI NtCreateSemaphore(PHANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES *, LONG, LONG);
__declspec(dllimport) NTSTATUS NTAPI NtReleaseSemaphore(HANDLE, LONG, PLONG);
__declspec(dllimport) NTSTATUS NTAPI NtResumeThread(HANDLE, PULONG);
__declspec(dllimport) NTSTATUS NTAPI NtSuspendThread(HANDLE, PULONG);
__declspec(dllimport) NTSTATUS NTAPI NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) NTSTATUS NTAPI NtDelayExecution(BOOLEAN, PLARGE_INTEGER);
__declspec(dllimport) NTSTATUS NTAPI NtYieldExecution(void);
__declspec(dllimport) NTSTATUS NTAPI NtDuplicateObject(HANDLE, HANDLE, HANDLE, PHANDLE, ACCESS_MASK, ULONG, ULONG);
__declspec(dllimport) ULONG NTAPI RtlNtStatusToDosError(NTSTATUS);
__declspec(dllimport) VOID NTAPI RtlTimeToTimeFields(PLARGE_INTEGER, TIME_FIELDS *);
__declspec(dllimport) BOOLEAN NTAPI RtlTimeFieldsToTime(TIME_FIELDS *, PLARGE_INTEGER);
__declspec(dllimport) VOID NTAPI RtlRaiseException(PEXCEPTION_RECORD);
}

} // namespace host

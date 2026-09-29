"""Windows job-object containment for fixed local build executables, without a shell."""

import asyncio
import ctypes
import os
import subprocess
from ctypes import wintypes

from .errors import JevError


class _BasicLimits(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_longlong),
        ("PerJobUserTimeLimit", ctypes.c_longlong),
        ("LimitFlags", wintypes.DWORD),
        ("MinimumWorkingSetSize", ctypes.c_size_t),
        ("MaximumWorkingSetSize", ctypes.c_size_t),
        ("ActiveProcessLimit", wintypes.DWORD),
        ("Affinity", ctypes.c_size_t),
        ("PriorityClass", wintypes.DWORD),
        ("SchedulingClass", wintypes.DWORD),
    ]


class _IoCounters(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_ulonglong)
        for name in (
            "ReadOperationCount",
            "WriteOperationCount",
            "OtherOperationCount",
            "ReadTransferCount",
            "WriteTransferCount",
            "OtherTransferCount",
        )
    ]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", _BasicLimits),
        ("IoInfo", _IoCounters),
        ("ProcessMemoryLimit", ctypes.c_size_t),
        ("JobMemoryLimit", ctypes.c_size_t),
        ("PeakProcessMemoryUsed", ctypes.c_size_t),
        ("PeakJobMemoryUsed", ctypes.c_size_t),
    ]


class ProcessTree:
    """Create suspended, contain before any child code runs, then resume."""

    def __init__(self):
        if os.name != "nt":
            raise JevError("unsupported_platform", "Named engine jobs currently require Windows.")
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
        self.kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        self.kernel.CreateJobObjectW.restype = wintypes.HANDLE
        self.kernel.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        self.kernel.SetInformationJobObject.restype = wintypes.BOOL
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        self.kernel.AssignProcessToJobObject.restype = wintypes.BOOL
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
        self.ntdll.NtResumeProcess.restype = wintypes.LONG
        self.handle = self.kernel.CreateJobObjectW(None, None)
        limits = _ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
        if not self.handle or not self.kernel.SetInformationJobObject(
            self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            self.close()
            raise JevError(
                "job_containment", "Cannot establish the required process-tree boundary."
            )

    def resume(self, pid: int):
        process = self.kernel.OpenProcess(0x0001 | 0x0100 | 0x0800 | 0x1000, False, pid)
        try:
            if not process or not self.kernel.AssignProcessToJobObject(self.handle, process):
                raise JevError("job_containment", "Cannot contain the suspended process tree.")
            if self.ntdll.NtResumeProcess(process) < 0:
                raise JevError("job_containment", "Cannot resume the contained process.")
        finally:
            if process:
                self.kernel.CloseHandle(process)

    def close(self):
        if getattr(self, "handle", None):
            self.kernel.CloseHandle(self.handle)
            self.handle = None


async def start_contained(command: list[str], cwd: str, env: dict[str, str]):
    tree = ProcessTree()
    process = None
    spawn = None
    try:
        spawn = asyncio.create_task(
            asyncio.create_subprocess_exec(
                *command,
                cwd=cwd,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.STDOUT,
                creationflags=0x00000004 | subprocess.CREATE_NO_WINDOW,  # CREATE_SUSPENDED
                limit=65536,
            )
        )
        process = await asyncio.shield(spawn)
        tree.resume(process.pid)
        return process, tree
    except BaseException:
        tree.close()
        if process is None and spawn is not None:
            try:
                process = await spawn
            except (OSError, asyncio.CancelledError):
                pass
        if process is not None:
            if process.returncode is None:
                process.kill()
            await process.wait()
        raise

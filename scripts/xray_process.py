"""Run a command with an owned process tree, including abnormal parent exits."""

import ctypes
from ctypes import wintypes
import os
import signal
import subprocess
import time


class RecoveryRequired(RuntimeError):
    pass


def run_child(command: list[str], *, timeout: float | None = None, **kwargs) -> int:
    """Wait for the leader; cleanup completes before a timeout is propagated."""
    if os.name != "nt":
        child = subprocess.Popen(command, start_new_session=True, **kwargs)
        try:
            return child.wait(timeout=timeout)
        finally:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
    return _run_windows(command, timeout=timeout, **kwargs)


def _run_windows(command: list[str], *, timeout: float | None = None, **kwargs) -> int:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    native = ctypes.WinDLL("ntdll")

    class BasicLimits(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", wintypes.DWORD), ("min_working", ctypes.c_size_t),
                    ("max_working", ctypes.c_size_t), ("active", wintypes.DWORD),
                    ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                    ("scheduling", wintypes.DWORD)]

    class Limits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", ctypes.c_uint64 * 6),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process", ctypes.c_size_t), ("peak_job", ctypes.c_size_t)]

    class Accounting(ctypes.Structure):
        _fields_ = [("times", ctypes.c_int64 * 4), ("faults", wintypes.DWORD),
                    ("total", wintypes.DWORD), ("active", wintypes.DWORD),
                    ("terminated", wintypes.DWORD)]

    kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    kernel.CreateJobObjectW.restype = wintypes.HANDLE
    kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    kernel.SetInformationJobObject.restype = wintypes.BOOL
    kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    kernel.AssignProcessToJobObject.restype = wintypes.BOOL
    kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel.TerminateJobObject.restype = wintypes.BOOL
    kernel.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                wintypes.DWORD, ctypes.c_void_p]
    kernel.QueryInformationJobObject.restype = wintypes.BOOL
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    native.NtResumeProcess.argtypes = [wintypes.HANDLE]
    native.NtResumeProcess.restype = ctypes.c_long
    job = kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    child = None
    assigned = False
    member_handles = []

    def retain_members():
        capacity = 32
        while True:
            if time.monotonic() >= deadline:
                raise RecoveryRequired("Command membership did not stabilize")
            class ProcessIds(ctypes.Structure):
                _fields_ = [("assigned", wintypes.DWORD), ("count", wintypes.DWORD),
                            ("ids", ctypes.c_size_t * capacity)]
            members = ProcessIds()
            if kernel.QueryInformationJobObject(job, 3, ctypes.byref(members),
                                               ctypes.sizeof(members), None):
                if members.count == members.assigned:
                    break
            elif ctypes.get_last_error() != 234:  # ERROR_MORE_DATA
                raise RecoveryRequired("Could not enumerate the command's process tree")
            capacity = max(capacity * 2, members.assigned)
        for pid in members.ids[:members.count]:
            handle = kernel.OpenProcess(0x100000, False, pid)  # SYNCHRONIZE
            if handle:
                member_handles.append(handle)
            elif ctypes.get_last_error() != 87:  # Already exited and released.
                raise RecoveryRequired("Could not retain a command process handle")
    try:
        limits = Limits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        child = subprocess.Popen(command, creationflags=0x00000004, **kwargs)  # CREATE_SUSPENDED
        if not kernel.AssignProcessToJobObject(job, int(child._handle)):
            raise ctypes.WinError(ctypes.get_last_error())
        assigned = True
        if native.NtResumeProcess(int(child._handle)) != 0:
            raise RuntimeError("Could not resume the job-owned command")
        return child.wait(timeout=timeout)
    finally:
        try:
            if child is not None:
                if assigned:
                    # A zero active count can precede a process handle becoming
                    # signaled. Retain the current members before termination
                    # so cleanup also waits for their actual exit notifications.
                    deadline = time.monotonic() + 30
                    retain_members()
                    if not kernel.TerminateJobObject(job, 1):
                        raise RecoveryRequired("Could not terminate the command's process tree")
                    while True:
                        accounting = Accounting()
                        if not kernel.QueryInformationJobObject(job, 1, ctypes.byref(accounting),
                                                                ctypes.sizeof(accounting), None):
                            raise RecoveryRequired("Could not inspect the command's process tree")
                        if accounting.active == 0:
                            break
                        if time.monotonic() >= deadline:
                            raise RecoveryRequired("Command descendants did not exit")
                        time.sleep(0.02)
                    for handle in member_handles:
                        remaining = max(0, int((deadline - time.monotonic()) * 1000))
                        if kernel.WaitForSingleObject(handle, remaining) != 0:
                            raise RecoveryRequired("A command process did not finish termination")
                elif child.poll() is None:
                    child.kill()
                child.wait()
        finally:
            for handle in member_handles:
                kernel.CloseHandle(handle)
            kernel.CloseHandle(job)

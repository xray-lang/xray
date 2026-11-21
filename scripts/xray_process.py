"""Run a command with an owned process tree, including abnormal parent exits."""

from contextlib import nullcontext
import ctypes
from ctypes import wintypes
import os
import signal
import subprocess
import sys
import time


class RecoveryRequired(RuntimeError):
    pass


GROUP_EXIT_SECONDS = 5.0


def _kill_group(group: int, deadline: float | None = None) -> bool:
    """Kill every remaining member of a command's POSIX process group.

    Darwin answers EPERM rather than ESRCH while the group holds only exiting
    processes that have not been reaped yet. Those are retried until the group
    disappears; a group that still refuses after the deadline is reported
    instead of being treated as gone.

    Return True if the signal was accepted, or False if the group was gone.
    """
    if deadline is None:
        deadline = time.monotonic() + GROUP_EXIT_SECONDS
    while True:
        try:
            os.killpg(group, signal.SIGKILL)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            if time.monotonic() >= deadline:
                raise RecoveryRequired("Command process group refused termination") from None
            time.sleep(0.01)


def _wait_darwin_group(group: int, deadline: float) -> None:
    """Prove that an asynchronously signalled group has disappeared."""
    while True:
        try:
            os.killpg(group, 0)
        except ProcessLookupError:
            return
        except PermissionError:
            # Darwin can report EPERM while the original group still exists.
            pass
        except OSError as error:
            raise RecoveryRequired("Cannot verify command process group termination") from error
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RecoveryRequired("Command process group did not disappear")
        time.sleep(min(0.01, remaining))


def run_child(command: list[str], *, timeout: float | None = None, spawn_guard=nullcontext, **kwargs) -> int:
    """Wait for the leader; cleanup completes before propagating a timeout.

    The optional context factory guards process creation only. It may raise
    to deny a launch and must not suppress launch errors.
    """
    if os.name != "nt":
        child = None
        try:
            with spawn_guard():
                child = subprocess.Popen(command, start_new_session=True, **kwargs)
            return child.wait(timeout=timeout)
        finally:
            if child is not None:
                deadline = time.monotonic() + GROUP_EXIT_SECONDS
                signalled = _kill_group(child.pid, deadline)
                child.wait()
                if signalled and sys.platform == "darwin":
                    _wait_darwin_group(child.pid, deadline)
    return _run_windows(command, timeout=timeout, spawn_guard=spawn_guard, **kwargs)


def _run_windows(command: list[str], *, timeout: float | None = None, spawn_guard=nullcontext, **kwargs) -> int:
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
        with spawn_guard():
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
                    before = Accounting()
                    if not kernel.QueryInformationJobObject(job, 1, ctypes.byref(before),
                                                            ctypes.sizeof(before), None):
                        raise RecoveryRequired("Could not inspect command membership before cleanup")
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
                    # New associations cannot be proved exited by the retained
                    # snapshot. TotalProcesses includes even short-lived members;
                    # do not release a successful execution boundary in this race.
                    if accounting.total != before.total:
                        raise RecoveryRequired(
                            "Command membership changed during cleanup "
                            f"({before.total} -> {accounting.total}); descendant exit is unverified")
                elif child.poll() is None:
                    child.kill()
                child.wait()
        finally:
            for handle in member_handles:
                kernel.CloseHandle(handle)
            kernel.CloseHandle(job)

"""Real Windows Job failures and hard parent death; kills only owned dummies."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from xraytest import windows_owned_job as job


def checked(condition, message):
    if not condition:
        raise RuntimeError(message)


def kernel():
    dll = job._kernel32()
    signatures = {
        "CreateMutexW": ([ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
        "GetProcessHandleCount": ([wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
        "OpenProcess": ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
        "WaitForSingleObject": ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
        "TerminateProcess": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
        "GetHandleInformation": ([wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
    }
    for name, (args, result) in signatures.items():
        call = getattr(dll, name)
        call.argtypes, call.restype = args, result
    return dll


class FailureApi:
    """Failures come from actual WinAPI invalid inputs, not fabricated returns."""
    def __init__(self, dll, phase):
        self.dll, self.phase, self.mutex = dll, phase, None

    def __getattr__(self, name):
        return getattr(self.dll, name)

    def CreateJobObjectW(self, security, name):
        if self.phase == "create":
            name = "Local\\xray-test-job-" + uuid.uuid4().hex
            self.mutex = self.dll.CreateMutexW(None, False, name)
            checked(bool(self.mutex), "Cannot create owned namespace collision control")
            # Mutex/Job names share the kernel namespace; this really fails.
        return self.dll.CreateJobObjectW(security, name)

    def SetHandleInformation(self, handle, mask, flags):
        return self.dll.SetHandleInformation(None if self.phase == "inherit" else handle, mask, flags)

    def SetInformationJobObject(self, handle, kind, information, size):
        return self.dll.SetInformationJobObject(handle, kind, information, 0 if self.phase == "limits" else size)

    def AssignProcessToJobObject(self, handle, process):
        return self.dll.AssignProcessToJobObject(handle, None if self.phase == "assign" else process)


def handle_count(dll):
    count = wintypes.DWORD()
    checked(bool(dll.GetProcessHandleCount(dll.GetCurrentProcess(), ctypes.byref(count))), "Handle count failed")
    return count.value


def failure_probe(phase):
    dll = kernel()
    before = handle_count(dll)
    api = FailureApi(dll, phase)
    caught = False
    try:
        job.establish_job(_kernel=api)
    except OSError:
        caught = True
    finally:
        if api.mutex:
            checked(bool(dll.CloseHandle(api.mutex)), "Owned mutex control close failed")
    after = handle_count(dll)
    checked(caught and job._owned_job is None, "Failed Job was published")
    checked(before == after, "Failed Job leaked a real handle")
    print(json.dumps({"phase": phase, "published": False, "handles_before": before, "handles_after": after}))


def dummy(root, index):
    grandchild = None
    if index == 0:
        grandchild = subprocess.Popen([sys.executable, __file__, "--dummy", str(root), "2"], close_fds=True)
    (root / f"dummy-{index}.json").write_text(json.dumps({"pid": os.getpid(),
        "grandchild": grandchild.pid if grandchild else None}), encoding="utf-8")
    while True:
        time.sleep(60)


def dummy_parent(root):
    dll = kernel()
    handle = job.establish_job()
    flags = wintypes.DWORD()
    checked(bool(dll.GetHandleInformation(handle, ctypes.byref(flags))) and not (flags.value & 1),
            "The sole Job handle must not be inherited")
    children = [subprocess.Popen([sys.executable, __file__, "--dummy", str(root), str(index)],
                                close_fds=True) for index in range(2)]
    (root / "parent.json").write_text(json.dumps({"parent": os.getpid(), "children": [c.pid for c in children],
        "non_inheritable": True}), encoding="utf-8")
    while True:
        time.sleep(60)


def wait_json(path, deadline):
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError:
                pass  # Only our dummy is still writing this uniquely owned file.
        time.sleep(0.01)
    raise RuntimeError(f"Owned dummy did not publish {path.name}")


def hard_parent_death():
    dll = kernel()
    handles = []
    with tempfile.TemporaryDirectory(prefix="xray-metadata-job-") as directory:
        root = Path(directory)
        parent = subprocess.Popen([sys.executable, __file__, "--parent", str(root)],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, close_fds=True)
        try:
            deadline = time.monotonic() + 10
            info = wait_json(root / "parent.json", deadline)
            checked(info["parent"] == parent.pid and info["non_inheritable"] is True, "Wrong dummy parent identity")
            rows = [wait_json(root / f"dummy-{i}.json", deadline) for i in range(3)]
            checked([rows[0]["pid"], rows[1]["pid"]] == info["children"] and rows[0]["grandchild"] == rows[2]["pid"],
                    "Unexpected owned dummy tree")
            for row in rows:
                handle = dll.OpenProcess(0x00100001, False, row["pid"])  # SYNCHRONIZE | TERMINATE
                checked(bool(handle), "Cannot hold the exact owned dummy process")
                handles.append(handle)
                checked(dll.WaitForSingleObject(handle, 0) == 0x102, "Dummy exited before hard parent kill")
            # This is precisely CTest's hard-termination failure mode. The test
            # must not manually terminate children before checking Job cleanup.
            parent.kill()
            parent.wait(timeout=5)
            for handle in handles:
                checked(dll.WaitForSingleObject(handle, 5000) == 0, "Parent death left an owned descendant alive")
            print("Windows Job hard parent kill released both owned children and grandchild PASS")
        finally:
            if parent.poll() is None:
                parent.kill()
                parent.wait(timeout=5)
            # Cleanup on a failing regression targets only exact held process
            # handles from our unique dummy tree, never arbitrary workspace PIDs.
            for handle in handles:
                if dll.WaitForSingleObject(handle, 0) == 0x102:
                    dll.TerminateProcess(handle, 1)
                    dll.WaitForSingleObject(handle, 5000)
                checked(bool(dll.CloseHandle(handle)), "Owned process handle close failed")


def main():
    checked(os.name == "nt", "This Windows-only regression must not silently skip")
    if len(sys.argv) == 3 and sys.argv[1] == "--failure":
        failure_probe(sys.argv[2])
        return 0
    if len(sys.argv) == 3 and sys.argv[1] == "--parent":
        dummy_parent(Path(sys.argv[2]))
    if len(sys.argv) == 4 and sys.argv[1] == "--dummy":
        dummy(Path(sys.argv[2]), int(sys.argv[3]))
    checked(len(sys.argv) == 1, "Unexpected regression arguments")
    # The regression runner also owns its complete dummy tree if CTest kills it.
    job.establish_job()
    for phase in ("create", "inherit", "limits", "assign"):
        result = subprocess.run([sys.executable, __file__, "--failure", phase],
                                capture_output=True, timeout=10, check=True)
        row = json.loads(result.stdout.decode("utf-8"))
        checked(row["phase"] == phase and row["published"] is False and row["handles_before"] == row["handles_after"],
                "Job failure lost its real cleanup evidence")
        print(result.stdout.decode("utf-8").strip())
    hard_parent_death()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

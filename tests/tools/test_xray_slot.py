#!/usr/bin/env python3
"""Exercise the actual resource scheduler with independent processes."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import ctypes
import shutil
import threading
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from xray_slot import FileLock, Lease, mutex, save_json

WORKER = """
from pathlib import Path
import sys, time
from xray_slot import Lease
root, kind, label, priority = sys.argv[1:]
with Lease(Path(root), kind, int(priority), timeout=15, payload={'label':label}):
    while not (Path(root) / ('release-' + label)).exists():
        time.sleep(0.02)
    (Path(root) / ('exit-attempted-' + label)).touch()
"""


class Slots(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="xray-slot-test-")
        self.root = Path(self.temporary.name)
        self.children = []
        self.child_logs = []
        self.environment = dict(os.environ, PYTHONPATH=str(SCRIPTS))

    def tearDown(self):
        for child in self.children:
            if child.poll() is None:
                child.kill()
            child.wait()
        for log in self.child_logs:
            log.close()
        for path in self.root.glob("worker-*.log"):
            diagnostic = path.read_text(encoding="utf-8")
            if diagnostic:
                print(f"\n{path.name}:\n{diagnostic}", file=sys.stderr)
        self.temporary.cleanup()

    def rows(self):
        return [json.loads(p.read_text(encoding="utf-8"))
                for p in (self.root / "requests").glob("*.json")]

    def wait(self, predicate):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.02)
        self.fail(f"scheduler did not reach expected state: {self.rows()}")

    def state(self, label):
        return next((r["state"] for r in self.rows() if r["payload"].get("label") == label), None)

    def start(self, label, kind="build", priority=1):
        log = (self.root / ("worker-" + label + ".log")).open("w", encoding="utf-8")
        self.child_logs.append(log)
        child = subprocess.Popen([sys.executable, "-c", WORKER, str(self.root), kind, label, str(priority)],
                                 env=self.environment, stdout=log, stderr=subprocess.STDOUT)
        self.children.append(child)
        self.wait(lambda: self.state(label) is not None)
        return child

    def release(self, label, child):
        (self.root / ("release-" + label)).touch()
        self.assertEqual(child.wait(timeout=5), 0)

    def test_capacity_priority_and_fifo(self):
        active = [self.start(str(n)) for n in range(3)]
        self.wait(lambda: all(self.state(str(n)) == "running" for n in range(3)))
        normal = self.start("normal")
        later = self.start("later")
        merge = self.start("merge", priority=0)
        self.assertEqual(self.state("normal"), "queued")
        self.assertEqual(self.state("merge"), "queued")
        self.release("0", active[0])
        self.wait(lambda: self.state("merge") == "running")
        self.assertEqual(self.state("normal"), "queued")
        self.release("merge", merge)
        self.wait(lambda: self.state("normal") == "running")
        self.assertEqual(self.state("later"), "queued")
        self.release("normal", normal)
        self.wait(lambda: self.state("later") == "running")
        self.release("later", later)
        for n in (1, 2):
            self.release(str(n), active[n])

    def test_heavy_is_one_slot_and_independent_of_build(self):
        first = self.start("heavy1", "heavy")
        self.wait(lambda: self.state("heavy1") == "running")
        second = self.start("heavy2", "heavy")
        build = self.start("build")
        self.wait(lambda: self.state("build") == "running")
        self.assertEqual(self.state("heavy2"), "queued")
        self.release("heavy1", first)
        self.wait(lambda: self.state("heavy2") == "running")
        self.release("heavy2", second)
        self.release("build", build)

    def test_nonzero_child_exit_is_preserved(self):
        result = subprocess.run([sys.executable, str(SCRIPTS / "xray_slot.py"), "--kind", "build",
                                 "--root", str(self.root), "--", sys.executable, "-c", "raise SystemExit(7)"],
                                env=self.environment, timeout=10)
        self.assertEqual(result.returncode, 7)
        self.assertEqual(self.rows()[0]["state"], "failed")
        child = self.start("next")
        self.wait(lambda: self.state("next") == "running")
        self.release("next", child)

    def test_killed_running_owner_requires_recovery(self):
        child = self.start("abandoned", "heavy")
        self.wait(lambda: self.state("abandoned") == "running")
        child.kill()
        child.wait()
        next_child = self.start("next", "heavy")
        self.assertNotEqual(next_child.wait(timeout=5), 0)
        self.assertEqual(self.state("abandoned"), "NEEDS_RECOVERY")

    def test_finishing_owner_cannot_be_reclassified_as_abandoned(self):
        child = self.start("finishing", "heavy")
        self.wait(lambda: self.state("finishing") == "running")
        request = next((self.root / "requests").glob("*.json"))
        acquire = FileLock.acquire

        def finish_after_record_read(lock):
            if lock.path == request.with_suffix(".live"):
                (self.root / "release-finishing").touch()
                self.wait(lambda: (self.root / "exit-attempted-finishing").exists())
                # A finishing owner must wait for this scheduler transaction.
                # Previously it released .live here, so the stale running row
                # was overwritten with NEEDS_RECOVERY by the scan below.
                try:
                    child.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
            return acquire(lock)

        with mutex(self.root / "scheduler.lock", time.monotonic() + 5):
            with mock.patch.object(FileLock, "acquire", finish_after_record_read):
                self.assertEqual(Lease(self.root, "heavy")._requests(), [])
        self.assertEqual(child.wait(timeout=5), 0)
        self.assertEqual(self.state("finishing"), "finished")

    @unittest.skipUnless(os.name == "nt", "Windows observer sharing modes")
    def test_atomic_status_update_waits_for_a_short_lived_observer(self):
        path = self.root / "observed.json"
        save_json(path, {"state": "running"})
        errors = []

        def publish():
            try:
                save_json(path, {"state": "finished"})
            except Exception as error:
                errors.append(error)

        with path.open("r", encoding="utf-8"):
            writer = threading.Thread(target=publish)
            writer.start()
            writer.join(timeout=0.1)
            waiting = writer.is_alive()
            self.assertEqual(json.loads(path.read_text())["state"], "running")
        writer.join(timeout=5)
        self.assertTrue(waiting, errors)
        self.assertFalse(writer.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(json.loads(path.read_text())["state"], "finished")
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    @unittest.skipUnless(os.name == "nt", "Windows observer sharing modes")
    def test_atomic_status_update_does_not_hide_a_persistent_denial(self):
        path = self.root / "observed.json"
        save_json(path, {"state": "running"})
        started = time.monotonic()
        with path.open("r", encoding="utf-8"):
            with self.assertRaises(PermissionError):
                save_json(path, {"state": "finished"}, timeout=0.1)
        self.assertLess(time.monotonic() - started, 2)
        self.assertEqual(json.loads(path.read_text())["state"], "running")
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    @unittest.skipUnless(os.name == "nt", "Windows lock open error codes")
    def test_invalid_lock_path_fails_instead_of_waiting_for_a_holder(self):
        with self.assertRaises(PermissionError) as result:
            FileLock(self.root).acquire()
        self.assertEqual(result.exception.winerror, 5)

    @unittest.skipUnless(os.name == "nt", "Windows process-tree ownership")
    def test_failed_command_cannot_leave_a_live_descendant(self):
        pidfile = self.root / "descendant.pid"
        command = ("import subprocess,sys,pathlib; "
                   "p=subprocess.Popen([sys.executable,'-c','import time; time.sleep(120)']); "
                   "pathlib.Path(sys.argv[1]).write_text(str(p.pid)); raise SystemExit(7)")
        result = subprocess.run([sys.executable, str(SCRIPTS / "xray_slot.py"), "--kind", "build",
                                 "--root", str(self.root), "--", sys.executable, "-c", command, str(pidfile)],
                                timeout=10)
        self.assertEqual(result.returncode, 7)
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        process = kernel.OpenProcess(0x00100000, False, int(pidfile.read_text()))
        if process:
            try:
                self.assertEqual(kernel.WaitForSingleObject(process, 0), 0)
            finally:
                kernel.CloseHandle(process)
        self.assertEqual(self.rows()[0]["state"], "failed")

    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell 7 entry point")
    def test_powershell_preserves_argv_and_exit_code(self):
        expected = ["", "two words", 'a"b', "trailing\\"]
        arguments = ["-c", "import json,sys; print(json.dumps(sys.argv[1:])); raise SystemExit(7)", *expected]
        argsfile = self.root / "arguments.json"
        argsfile.write_text(json.dumps(arguments), encoding="utf-8")
        wrapper = self.root / "invoke.ps1"
        def literal(value):
            return "'" + str(value).replace("'", "''") + "'"
        wrapper.write_text(
            "$items = @(Get-Content -Raw -LiteralPath " + literal(argsfile) + " | ConvertFrom-Json)\n"
            "& " + literal(SCRIPTS / "xray-slot.ps1") + " -Kind build -SlotRoot " + literal(self.root)
            + " -Program " + literal(sys.executable) + " -ArgumentList $items\nexit $LASTEXITCODE\n",
            encoding="utf-8")
        result = subprocess.run(["pwsh", "-NoProfile", "-File", str(wrapper)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(json.loads(result.stdout), expected)

    @unittest.skipUnless(os.name == "nt" and shutil.which("pwsh"), "Windows legacy lock interop")
    def test_existing_fileshare_none_holder_is_respected(self):
        wrapper = self.root / "legacy.ps1"
        wrapper.write_text("param($Root)\n"
                           "$held=[IO.File]::Open((Join-Path $Root 'heavy-0.lock'), 'OpenOrCreate', 'ReadWrite', 'None')\n"
                           "try { Set-Content (Join-Path $Root 'legacy-ready') 'ready'; "
                           "while (-not (Test-Path (Join-Path $Root 'release-legacy'))) { Start-Sleep -Milliseconds 20 } }\n"
                           "finally { $held.Dispose() }\n", encoding="utf-8")
        legacy = subprocess.Popen(["pwsh", "-NoProfile", "-File", str(wrapper), str(self.root)],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(legacy)
        self.wait(lambda: (self.root / "legacy-ready").exists())
        child = self.start("new", "heavy")
        time.sleep(0.15)
        self.assertIsNone(child.poll())
        self.assertEqual(self.state("new"), "queued")
        self.release("legacy", legacy)
        self.wait(lambda: self.state("new") == "running")
        self.release("new", child)

    @unittest.skipUnless(os.name == "nt" and shutil.which("pwsh"), "Windows legacy lock interop")
    def test_new_holder_excludes_legacy_fileshare_none(self):
        child = self.start("new", "heavy")
        self.wait(lambda: self.state("new") == "running")
        wrapper = self.root / "probe.ps1"
        wrapper.write_text("param($Path)\ntry { $held=[IO.File]::Open($Path, 'OpenOrCreate', 'ReadWrite', 'None'); "
                           "$held.Dispose(); exit 0 } catch [IO.IOException] { exit 9 }\n", encoding="utf-8")
        args = ["pwsh", "-NoProfile", "-File", str(wrapper), str(self.root / "heavy-0.lock")]
        self.assertEqual(subprocess.run(args, timeout=10).returncode, 9)
        self.release("new", child)
        self.assertEqual(subprocess.run(args, timeout=10).returncode, 0)


if __name__ == "__main__":
    unittest.main()

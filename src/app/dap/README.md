# DAP stdio execution and retained output

`xray dap --stdio` reads and writes Content-Length framed DAP JSON. Target bytes never become unframed protocol stdout.

On Windows the command starts one hidden worker process. That worker owns the canonical Source compiler, Program and Instance from the beginning of the session. A detach continues that same execution, including an unstarted entry, stopped frames, child tasks and pending timers. It does not rerun the program or grant another compilation or execution budget.

The successful initialize response includes `supportTerminateDebuggee: true` and an `xrayRetainedOutput` extension:

```json
{"stdout":"C:\\...\\stdout.bin","stderr":"C:\\...\\stderr.bin","processId":1234}
```

The paths are the actual absolute endpoints for this session. The process ID identifies the executing worker, which differs from the frontend process. Each original target output group is written to its retained stream before its normal DAP output event while connected. After detach it continues to the same files. These are binary files: UTF-8 and embedded NUL bytes are preserved, and stdout and stderr remain separate. No separators or replayed output are added on detach or restart.

The session directory is created beneath the Windows temporary directory with an explicit protected DACL granting the current user access. The output files inherit this access policy and are created without overwriting existing files. Host diagnostics use the separate `host.log`. The files remain available after normal completion, detach or failure; the processes close their owned handles without automatically deleting retained user output. A startup or protocol failure reports the host diagnostic path when available.

For launch, disconnect without an explicit policy terminates execution. For attach, its default is to detach the admitted execution. Explicit `terminateDebuggee: false` detaches; `true` terminates. Invalid policy types are rejected. Nonempty program arguments, working-directory and environment options still require their own admitted per-Instance host provider and are rejected when unsupported.

Before detach commits, the frontend owns the worker's process group. Killing the frontend at this point also terminates the worker. The host release commits before observation is disabled and before a successful disconnect response is written. A rejected release preserves the original debugger state; a fatal host-control error is an I/O failure rather than a successful rejection that can be retried. Once committed, a response write failure does not revoke the transferred execution.

The worker sends the disconnect response and terminated event, closes its protocol stdin/stdout, and drives the retained execution to completion. The frontend releases its owner and exits without waiting for or terminating the released worker. Its successful exit confirms the handoff, not the target's final result. There is no later DAP exited event on that closed connection. The worker retains its real exit status, and target or output failures remain failures.

The independent Windows stdio consumer is covered by focused local tests. Full fault-injection and sanitizer qualification remain required. POSIX owned release is currently unsupported, so POSIX stdio does not advertise this detach capability. TCP uses the same canonical execution backend with its existing host-owned output lifetime. Native first-chance debugging and per-Instance host options are separate unfinished capabilities.

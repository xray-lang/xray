# XIR managed value boundary

This contract adds internal string values and explicit result ownership to the
resumable runtime. It does not qualify source syntax, Program/Instance sealing,
generic library publication, concurrent activation driving, or product cutover.

Value ABI 4 replaces prior value ABIs without an alias or reader. A value remains a
16-byte, eight-aligned carrier: u32 type, zero reserved u32, and eight payload
bytes. Unit/bool/i64 retain their canonical meanings. String payload bytes encode
a live opaque pointer by memcpy; they are never language integers. Host values
must originate from constructors or owned copies. This trusted native interface
does not validate forged/dangling pointers or accept serialized pointer bytes.

Strings own strict UTF-8, including embedded NUL. No normalization or replacement
occurs. Length queries distinguish bytes from Unicode scalar count, neither is a
grapheme count. Borrowed byte views expire on owner mutation/drop. Copy retains
atomic storage; drop releases it exactly once. Counts cannot wrap or resurrect:
retain saturation fails, and invalid release is a hard invariant failure. Atomic
RC does not permit racing access to one mutable value handle. Independent owned
copies may be read/dropped concurrently; publication synchronization is external.

An explicit refcounted allocation domain owns its limit and physical accounting.
Each string keeps only this small domain alive, never an activation, instance,
artifact, or code image. Domain admission charges metadata and byte capacity;
allocation failure rolls back reservations. The domain's host handle may be
dropped before strings. Its final string releases remaining storage physically.
Append requires exclusive access to the destination handle. Unique storage grows
in place when capacity permits and reallocates its byte buffer otherwise; shared
storage separates. Self-append is valid. Failure preserves the original value,
published copies, and live accounting. Capacity is an implementation detail.

Call ABI 7 admits strings and Atomic<i64> identity values. Arguments are borrowed at admission and copied into
owned frame storage before publication. Resume arguments/inbox and action return
values are borrowed views. The driver retains a return before child cleanup and
owns each inbox and terminal result. Poll exposes a borrowed result; explicit
take transfers it once and changes the activation to CONSUMED. Destruction drops
an untaken result. Taking a result needs no allocation and the resulting owned
value survives activation and input destruction. Frame cleanup runs before its
arguments and inbox are dropped. Failure/cancellation publishes no partial result.

Typed output is an explicit synchronous provider effect. Call ABI 7 replaces
the scalar callback with a borrowed typed group. Raw output has one value and
names stdout or stderr. Line output names stdout and carries zero or more
bool/i64/string values: arguments evaluate left to right before one provider call.
Rendering inserts exactly one ASCII space between values and one final LF, as
required by source print. A raw group adds neither spacing nor a newline.
The renderer validates and allocates the complete group before calling its byte
sink exactly once; malformed values, budget failure and OOM cause no sink call.
Embedded NUL and UTF-8 bytes are length-delimited. Values remain typed until the
provider chooses rendering. Missing/rejecting providers fail with OUTPUT_ERROR, not a
language throw or fallback. Reentrant cancellation takes effect after the callback;
already accepted output is not rolled back. The provider cannot suspend and must
copy any value it retains. Provider context is borrowed for activation lifetime.
Both native groups and XIR PRINT admit up to 65536 values within resource budgets.
PRINT uses a first/count range into the owned function operand table and a zero
immediate. Each operand must dominate the instruction and have bool/i64/string
type. Empty ranges are canonical zero/zero; nonempty ranges partition the table
in instruction order. Lowering reserves the exact maximum outgoing group capacity
in the stable activation frame and includes it in physical-byte admission.
Older call ABI providers and generated entries are rejected, with no adapter.

WRITE_STREAM is bool-typed, borrows exactly one string and names stdout/stderr
with immediate 1/2. It publishes one raw group through the instance output provider
and receives a canonical bool inbox before execution continues. A configured
provider's false result is an observable language value, while an absent provider
remains OUTPUT_ERROR. PRINT and OUTPUT still fail on provider rejection.
Cancellation requested by the provider takes precedence over either bool result;
accepted bytes cannot be rolled back. Provider calls cannot suspend. Native action
admission rejects line groups, non-string values, invalid streams/counts and dirty
unit payloads before calling the provider. Old call ABI entries are rejected.
This is the typed write-result primitive; source stdlib binding, host stream
flushing and filesystem short-write policy require their own implementation.

verification-test: test_xir_values
verification-test: test_xir_value_allocations

String XIR operations admit string parameters/results, abstract COPY before
lowering, physical OWNED_RETAIN after lowering, CONCAT_STRING, and OUTPUT.
OUTPUT is unit-typed, consumes a bool/i64/string operand, and uses immediate 1/2 for
stdout/stderr. CONCAT_STRING preserves both borrowed operands and returns an owned
string. Lowering publishes and reverifies an exact list of owned frame offsets.
Every string slot starts empty, owns its current value, drops before replacement,
and is cleared on all exits in reverse slot order. Both backends consume this
cleanup list. Parameters and child results are retained into slots; return actions
borrow until the driver captures ownership. Repeated loop definitions replace
and release previous values. Closed scalar leaf execution rejects managed values.
Literal tables are sealed with Program metadata. Source-to-XIR string production remains to be implemented.

verification-test: test_xir_string_vm
verification-test: test_xir_string_native
verification-test: test_xir_emit_strings
verification-test: test_xir_emit_output
verification-test: test_xir_output_vm
verification-test: test_xir_output_native

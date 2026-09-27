# XIR typed shared capture cells

An internal cell type is a CELL node in the same owned constructed pool as
CALLABLE and ARRAY, as specified by `xir-constructed-types.md`. Its element is
an admitted non-unit ordinary value type or an in-scope generic parameter,
never another cell. A constructed element is interned before the CELL node.
Kind and element come from that pool; former bit-encoded cell types reject.
Cells are implementation capabilities, not a source type or a Sendable proof.
Callable signatures and module slots cannot expose cell types. Private function
parameters may carry them as captured environment prefixes. Checked substitution
preserves the cell wrapper and rechecks its element; Lowered admits only closed
elements. CELL_NEW copies an initial value, CELL_READ produces an owned logical
copy, and CELL_WRITE copies before exchanging and dropping the previous value.
CELL_NEW carries the already interned CELL ID; execution does not register or
derive a new type. A runtime cell owns its TypeArena, and cell argument admission
requires the expected arena as well as the local type ID. A descriptor allowing
an ARRAY element does not implement runtime Array values or mutation operations.
The expression's value is separate from its shared storage identity.

All aliases observe the same mutable storage. Cell RC is atomic; content access
uses the instance's existing single-host-thread admission, not atomic mutation
or a cross-worker sharing promise. Function contents and captured cells belong
to the same value domain as the enclosing instance. Reading, constructing or
updating a cell cannot transfer a callable capability across instances. Arena
metadata keeps no reverse Program or Instance lease; contained functions retain
their own existing execution-admission and code owners.

Construction and reads publish only on success. Writes leave the old content
intact on OOM, budget, type, provenance or refcount failure. Last-reference
release queues the contained value on the existing allocation-free destruction
worklist; it never recursively destroys an environment chain. Ordinary strong
cycles remain possible and require explicit breaking while an owner remains.
Unreachable strong-cycle reclamation is OPEN: no collector, automatic cycle
reclamation or universal leak-freedom claim is introduced by this contract.

Value9/Call13/Program8 and schema5/semantic14 replace previous admission versions
without compatibility readers. Source var production must preserve aliases;
capturing a snapshot in place of a cell is forbidden.

verification-test: test_xir_values
verification-test: test_xir_value_allocations
verification-test: test_xir_checked
verification-test: test_xir_source_generics
verification-test: test_xir_source
verification-test: test_xir_source_native
verification-test: test_xir_source_mixed
verification-test: test_xir_packet_vm
verification-test: test_xir_source_allocations

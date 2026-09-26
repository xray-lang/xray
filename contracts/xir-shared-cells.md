# XIR typed shared capture cells

An internal cell type is 0x40000000 OR its element type. Elements are non-unit
ordinary value types or an in-scope generic parameter, never another cell.
Cells are implementation capabilities, not a source type or a Sendable proof.
Callable signatures and module slots cannot expose cell types. Private function
parameters may carry them as captured environment prefixes. Checked substitution
preserves the cell wrapper and rechecks its element; Lowered admits only closed
elements. CELL_NEW copies an initial value, CELL_READ produces an owned logical
copy, and CELL_WRITE copies before exchanging and dropping the previous value.
The expression's value is separate from its shared storage identity.

All aliases observe the same mutable storage. Cell RC is atomic; content access
uses the instance's existing single-host-thread admission, not atomic mutation
or a cross-worker sharing promise. Function contents and captured cells belong
to the same value domain as the enclosing instance. Reading, constructing or
updating a cell cannot transfer a callable capability across instances.

Construction and reads publish only on success. Writes leave the old content
intact on OOM, budget, type, provenance or refcount failure. Last-reference
release queues the contained value on the existing allocation-free destruction
worklist; it never recursively destroys an environment chain. Ordinary strong
cycles remain possible and require explicit breaking while an owner remains.
Unreachable strong-cycle reclamation is OPEN: no collector, automatic cycle
reclamation or universal leak-freedom claim is introduced by this contract.

Value8/Call12/Program7 and schema4/semantic13 replace previous admission versions
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

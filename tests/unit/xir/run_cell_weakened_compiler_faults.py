"""Complete the shared owned-Cell Source fixture's ten-stage compiler failure tree."""
import run_receiver_compiler_faults as compiler
compiler.owned.SCHEMA='cell-weakened-compiler-faults.v1'
compiler.FIXTURE_PATH='tests/unit/xir/xir_generator_cell_weakened_text.h'
compiler.FIXTURE_SYMBOL='cell_weakened_text'
compiler.FIXTURE_INPUTS=[compiler.FIXTURE_PATH,'tests/unit/xir/xir_generator_compiler_fault_cases.h',
                       'tests/unit/xir/test_xir_generator_cell_weakened_compiler.c',
                       'tests/unit/xir/run_cell_weakened_compiler_faults.py']
if __name__=='__main__':raise SystemExit(compiler.main())

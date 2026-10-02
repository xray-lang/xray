"""Independent Windows x86_64 C ABI layout oracle and actual compiler probe."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

PRIMITIVES = {'u32': (4, 4), 'i32': (4, 4), 'u64': (8, 8), 'ptr': (8, 8),
              'bool': (1, 1), 'enum': (4, 4)}
STRUCTURES = [
 (100, 'XrXirValue', [('type','u32'),('reserved','u32'),('payload','u64')]),
 (200, 'XrXirFaultDetail', [('code','u32'),('reserved','u32'),('index','u64'),('length','u64')]),
 (300, 'XrXirPanicPayload', [('detail','XrXirFaultDetail'),('message','XrXirValue')]),
 (400, 'XrXirCallResult', [('status','enum'),('value','XrXirValue'),('wake','u64'),('panic','XrXirPanicPayload')]),
 (500, 'XrXirAction', [('kind','enum'),('callee','u32'),('arguments','ptr'),('argument_count','u32'),
                      ('value','XrXirValue'),('panic','XrXirPanicPayload'),('flags','u32')]),
 (600, 'XrXirCallView', [('activation','ptr'),('instance','ptr'),('environment','ptr'),('state','ptr'),
                      ('arguments','ptr'),('argument_count','u32'),('inbox','XrXirCallResult'),
                      ('arena','ptr'),('phase','enum'),('exit','XrXirCallResult'),('scope_exit','bool')]),
 (700, 'XrXirCallEntry', [('abi_version','u32'),('parameters','ptr'),('parameter_count','u32'),('result','enum'),
                       ('state_bytes','u32'),('resume','ptr'),('release','ptr'),('environment','ptr'),
                       ('flags','u32'),('cleanup_owner','u32')]),
 (800, 'XrXirOutputProvider', [('abi_version','u32'),('reserved','u32'),('write','ptr'),('context','ptr')]),
 (900, 'XrXirValueAdmission', [('arena','ptr'),('domain','ptr'),('function','ptr'),('context','ptr'),
                             ('work','u64'),('scratch_bytes','u64')]),
 (1000, 'XrXirCallConfig', [('abi_version','u32'),('struct_size','u32'),('entries','ptr'),('entry_count','u32'),
                         ('instance','ptr'),('byte_limit','u64'),('poll_limit','u64'),('depth_limit','u32'),
                         ('accounting','ptr'),('output','XrXirOutputProvider'),('admission','XrXirValueAdmission')]),
 (1100, 'XrXirOutputSink', [('abi_version','u32'),('reserved','u32'),('write','ptr'),('context','ptr'),('byte_limit','u64')]),
 (1200, 'XrXirInstanceConfig', [('abi_version','u32'),('struct_size','u32'),('metadata_limit','u64'),('value_limit','u64'),
                             ('call_limit','u64'),('poll_limit','u64'),('depth_limit','u32'),
                             ('output','XrXirOutputProvider'),('trace','ptr'),('trace_context','ptr')]),
 (1300, 'XrXirInstanceResult', [('outcome','XrXirCallResult'),('epoch','u64')]),
 (1400, 'XrXirCodeLease', [('owner','ptr'),('release','ptr')]),
 (1500, 'XrXirProgramProof', [('bytes','ptr'),('length','u64'),('identity','ptr'),('layouts','ptr')]),
 (1600, 'XrXirTarget', [('architecture','u32'),('abi_version','u32')]),
 (1700, 'XrXirProgramSpec', [('abi_version','u32'),('target','XrXirTarget'),('entries','ptr'),('entry_count','u32'),
                          ('declarations','ptr'),('code','XrXirCodeLease'),('types','ptr'),('proof','XrXirProgramProof')]),
 (1800, 'XrXirLayout', [('size','u32'),('alignment','u32')]),
 (1900, 'XrXirFunctionLayout', [('slot_count','u32'),('frame_bytes','u32'),('offsets','ptr'),('parameters','ptr'),
                             ('result','XrXirLayout'),('owned_count','u32'),('owned_offsets','ptr'),
                             ('outgoing_count','u32'),('path_count','u32')]),
 (2000, 'XrXirOutputGroup', [('stream','enum'),('values','ptr'),('count','u32'),('line','bool')]),
 (2100, 'XrXirHostExecutionRequest', [('program','ptr'),('config','ptr'),('entry','u32'),('arguments','ptr'),('argument_count','u32')]),
 (2200, 'XrErrorCoreMessageView', [('code','i32'),('message','ptr'),('message_len','u64'),('has_code','bool')]),
]
SCALARS = [
 (1,'pointer.size',8,'sizeof(void *)'),(2,'pointer.align',8,'_Alignof(void *)'),
 (3,'size_t.size',8,'sizeof(size_t)'),(4,'size_t.align',8,'_Alignof(size_t)'),
 (5,'bool.size',1,'sizeof(bool)'),(6,'bool.align',1,'_Alignof(bool)'),
 (7,'u32.size',4,'sizeof(uint32_t)'),(8,'u32.align',4,'_Alignof(uint32_t)'),
 (9,'i64.size',8,'sizeof(int64_t)'),(10,'i64.align',8,'_Alignof(int64_t)'),
 (11,'f32.size',4,'sizeof(float)'),(12,'f32.align',4,'_Alignof(float)'),
 (13,'f64.size',8,'sizeof(double)'),(14,'f64.align',8,'_Alignof(double)'),
 (15,'CHAR_BIT',8,'CHAR_BIT'),(16,'little_endian',1,'*(const unsigned char *)&endian'),
 (17,'resume_pointer.size',8,'sizeof(XrXirResumeEntry)'),(18,'resume_pointer.align',8,'_Alignof(XrXirResumeEntry)'),
 (19,'CallStatus.size',4,'sizeof(XrXirCallStatus)'),(20,'CallStatus.align',4,'_Alignof(XrXirCallStatus)'),
 (21,'Type.size',4,'sizeof(XrXirType)'),(22,'Type.align',4,'_Alignof(XrXirType)'),
 (23,'OutputStatus.size',4,'sizeof(XrXirOutputStatus)'),(24,'OutputStatus.align',4,'_Alignof(XrXirOutputStatus)'),
 (25,'ActionKind.size',4,'sizeof(XrXirActionKind)'),(26,'ActionKind.align',4,'_Alignof(XrXirActionKind)'),
 (27,'CallPhase.size',4,'sizeof(XrXirCallPhase)'),(28,'CallPhase.align',4,'_Alignof(XrXirCallPhase)'),
 (29,'FLT_RADIX',2,'FLT_RADIX'),(30,'FLT_MANT_DIG',24,'FLT_MANT_DIG'),
 (31,'FLT_MAX_EXP',128,'FLT_MAX_EXP'),(32,'DBL_MANT_DIG',53,'DBL_MANT_DIG'),
 (33,'DBL_MAX_EXP',1024,'DBL_MAX_EXP'),(34,'frame_alignment',16,'XR_XIR_CALL_STATE_ALIGNMENT'),
]

def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment

def prepare(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=False)
    layout = dict(PRIMITIVES)
    rows = [{'id':i,'name':name,'value':value,'expression':expression} for i,name,value,expression in SCALARS]
    for base, name, fields in STRUCTURES:
        offsets = []; size = 0; alignment = 1
        for field, kind in fields:
            width, boundary = layout[kind]
            alignment = max(alignment, boundary)
            size = align(size, boundary); offsets.append((field, size));size += width
        size = align(size, alignment);layout[name] = (size, alignment)
        rows += [{'id':base,'name':f'{name}.size','value':size,'expression':f'sizeof({name})'},
                 {'id':base+1,'name':f'{name}.align','value':alignment,'expression':f'_Alignof({name})'}]
        rows += [{'id':base+2+i,'name':f'{name}.{field}.offset','value':offset,
                  'expression':f'offsetof({name},{field})'} for i,(field,offset) in enumerate(offsets)]
    document = {'recipe':'xray:xir-runtime-abi-measurements:v1','rows':rows,
                'basis':'Independent natural Windows x86_64 C layout: pointer/size_t/u64 align8, '
                        'u32/int/enum align4, bool align1; declaration field sequence copied '
                        'from actual versioned runtime headers. Never from a compiler probe output.'}
    (output/'EXPECTED.json').write_text(json.dumps(document,indent=2)+'\n',encoding='utf-8',newline='\n')
    source = '#include "xir/xxir_program.h"\n#include "xir/xxir_output.h"\n#include "execution/xr_xir_host_execution.h"\n#include <limits.h>\n#include <float.h>\n#include <stdio.h>\nint main(void) { const uint32_t endian=1; puts("[");\n'
    for index, row in enumerate(rows):
        tail = ',' if index+1 < len(rows) else ''
        source += f'printf("  {{\\"id\\":{row["id"]},\\"value\\":%u}}{tail}\\n",(unsigned)({row["expression"]}));\n'
    source += 'puts("]");return 0;}\n'
    (output/'abi_probe.c').write_text(source,encoding='utf-8',newline='\n')

def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True)
    args=parser.parse_args();prepare(Path(args.output))

if __name__=='__main__': main()

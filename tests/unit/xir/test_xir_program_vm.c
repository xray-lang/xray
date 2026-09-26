/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_vm.c - Sealed Lowered programs in the VM
 *
 * KEY CONCEPT:
 *   Instance code owns its artifact and matches independent expected effects.
 */
#include "xir/xxir_vm.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_program_fixture.h"
#include "xir_program_cases.h"
#include "xir_capture_fixture.h"
#include "xir_capture_cases.h"
static void admission(void) {
    for (uint32_t invalid = 0; invalid < 23; ++invalid) {
        XrXirArtifact *artifact = program_fixture(0), *saved = artifact;
        const XrXirModule *module = xr_xir_artifact_module(artifact);
        XrXirDeclarations *d = (XrXirDeclarations *) module->declarations;
        XrXirSourceModule *modules = (XrXirSourceModule *) d->modules;
        XrXirFunctionIdentity *ids = (XrXirFunctionIdentity *) d->functions;
        XrXirSlot *slots = (XrXirSlot *) d->slots;
        XrXirLiteral *literals = (XrXirLiteral *) d->literals;
        XrXirInstruction *root = (XrXirInstruction *) module->functions[3].instructions;
        XrXirInstruction *alpha = (XrXirInstruction *) module->functions[2].instructions;
        switch (invalid) {
        case 0: ((uint32_t *) modules[0].dependencies)[1] = 1; break;
        case 1: ((uint32_t *) modules[0].dependencies)[1] = 0; break;
        case 2: ((uint32_t *) modules[0].dependencies)[1] = 3; break;
        case 3: modules[0].dependency_count = 1; break;
        case 4: memcpy((char *) modules[1].name, "root", 4); break;
        case 5: ((unsigned char *) modules[1].name)[0] = 0xff; break;
        case 6: ids[4].exported = 0; break;
        case 7: ids[2].exported = 1; break;
        case 8: d->entry_function = 2; break;
        case 9: slots[0].mutable = 1; break;
        case 10: slots[4].type = XR_XIR_ATOMIC_I64; break;
        case 11: ((unsigned char *) literals[0].bytes)[0] = 0xff; break;
        case 12: alpha[3].immediate = 5; break;
        case 13: alpha[2].immediate = 1; break;
        case 14: alpha[2].args[0] = 0; break;
        case 15: root[9].op = XR_XIR_SLOT_INIT; break;
        case 16: root[0].immediate = 2; break;
        case 17: root[6].type = XR_XIR_I64; break;
        case 18: alpha[1].args[0] = 3; break;
        case 19: alpha[5].args[0] = 1; break;
        case 20: ((XrXirInstruction *) module->functions[4].instructions)[2].args[1] = 0; break;
        case 21: root[9].immediate = 0; break;
        case 22: ids[4].exported = 2; break;
        }
        XrXirProgram *program = NULL;
        CHECK(xr_xir_artifact_verify(artifact, NULL, NULL) != XR_XIR_OK);
        CHECK(xr_xir_vm_program_take(&artifact, 65536, &program) != XR_XIR_OK);
        CHECK(artifact == saved && !program);
        xr_xir_artifact_free(artifact);
    }
}
extern const XrXirProgramSpec captures_program;
typedef struct CaptureMixedOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding bindings[6];
    XrXirCallEntry entries[6];
} CaptureMixedOwner;
static unsigned mixed_releases;
static void capture_mixed_release(void *pointer) {
    CaptureMixedOwner *owner = pointer;
    xr_xir_artifact_free(owner->artifact); xr_free(owner); ++mixed_releases;
}
static void capture_mixed(void) {
    for (unsigned parity = 0; parity < 2; ++parity) {
        CaptureMixedOwner *owner = xr_calloc(1,sizeof(*owner)); CHECK(owner);
        owner->artifact = capture_fixture();
        const XrXirModule *module = xr_xir_artifact_module(owner->artifact);
        for (uint32_t i = 0; i < 6; ++i) {
            CHECK(xr_xir_vm_bind(owner->artifact,i,&owner->bindings[i],&owner->entries[i]) == XR_XIR_OK);
            if ((i == 3 ? 0u : 1u) == parity) owner->entries[i] = captures_program.entries[i];
        }
        CHECK((owner->entries[3].resume == captures_program.entries[3].resume) !=
            (owner->entries[5].resume == captures_program.entries[5].resume));
        XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION,
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},owner->entries,6,module->declarations,
            {owner,capture_mixed_release},module->callables};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_program_seal(&spec,65536,&program) == XR_XIR_OK);
        capture_cases(program); CHECK(mixed_releases == parity+1);
    }
}
int main(void) {
    admission(); capture_mixed();
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirArtifact *artifact = program_fixture(mode), *saved = artifact;
        XrXirProgram *program = NULL;
        CHECK(xr_xir_vm_program_take(&artifact, 1, &program) == XR_XIR_BUDGET);
        CHECK(artifact == saved && !program);
        CHECK(xr_xir_vm_program_take(&artifact, 65536, &program) == XR_XIR_OK);
        CHECK(!artifact && program);
        program_cases(program, mode);
    }
    XrXirArtifact *artifact = capture_fixture();
    XrXirProgram *captures = NULL;
    CHECK(xr_xir_vm_program_take(&artifact,65536,&captures) == XR_XIR_OK);
    capture_cases(captures);
    puts("VM capture environments, two suspensions, cancellation and escaped ownership passed");
    puts("VM module programs match independent output, state and lifetime expectations");
    return 0;
}

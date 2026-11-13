/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "base/xsha256.h"
static uint32_t lexical_packet_u32(const XrXirCheckedPacket *packet,size_t at) {
    CHECK(at<=packet->length && packet->length-at>=4);
    const uint8_t *p=packet->bytes+at;
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void lexical_packet_put(XrXirCheckedPacket *packet,size_t at,uint32_t value) {
    CHECK(at<=packet->length && packet->length-at>=4);
    for (unsigned n=0;n<4;++n) packet->bytes[at+n]=(uint8_t)(value>>(n*8));
}
static void lexical_packet_digest(XrXirCheckedPacket *packet) {
    XrSHA256Context hash;xr_sha256_init(&hash);
    xr_sha256_update(&hash,packet->bytes,32);
    xr_sha256_update(&hash,packet->bytes+64,packet->length-64);
    xr_sha256_final(&hash,packet->bytes+32);
}
static void lexical_packet_boundaries(XrXirCheckedPacket *packet,const XirLexicalProgram *program) {
    uint64_t bytes=4;
    for (uint32_t f=0;f<program->function_count;++f) {
        const XirLexicalFunction *row=&program->functions[f];
        bytes+=12u+8u*(uint64_t)row->scope_count+56u*(uint64_t)row->binding_count+
            52u*(uint64_t)row->event_count;
    }
    CHECK(bytes<=packet->length);
    size_t at=packet->length-(size_t)bytes,first=SIZE_MAX;
    CHECK(lexical_packet_u32(packet,at)==program->function_count);at+=4;
    for (uint32_t f=0;f<program->function_count;++f) {
        const XirLexicalFunction *row=&program->functions[f];
        CHECK(lexical_packet_u32(packet,at)==row->scope_count);at+=4+8u*(size_t)row->scope_count;
        CHECK(lexical_packet_u32(packet,at)==row->binding_count);at+=4+56u*(size_t)row->binding_count;
        CHECK(lexical_packet_u32(packet,at)==row->event_count);at+=4;
        if (first==SIZE_MAX && row->event_count) first=at;
        at+=52u*(size_t)row->event_count;
    }
    CHECK(at==packet->length && first!=SIZE_MAX);
    const size_t offsets[]={first+8,first+44};
    const uint32_t replacements[]={UINT32_MAX,UINT32_MAX};
    EffectMark retained=effect_mark();
    for (unsigned n=0;n<2;++n) {
        uint32_t saved=lexical_packet_u32(packet,offsets[n]);
        lexical_packet_put(packet,offsets[n],replacements[n]);lexical_packet_digest(packet);
        XrXirArtifact *denied=NULL;
        CHECK(xr_xir_compile_checked_read(&effect_context,packet->bytes,packet->length,&denied,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(!denied);effect_mark_check(retained);
        lexical_packet_put(packet,offsets[n],saved);lexical_packet_digest(packet);
    }
}
static void lexical_source_expectations(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const char *names[]={"lexicalBlocks","lexicalLoop","lexicalCleanup","lexicalGeneric",
        "lexicalFactory","lexicalFnExtents","lexicalView","lexicalPanic"};
    for (unsigned n=0;n<8;++n) {
        uint32_t f=root_find(module,names[n]);
        XirLexicalFunction directory={0};
        CHECK(xr_xir_compile_lexical_function(artifact,f,&directory)==XR_XIR_OK);
        CHECK(directory.scope_count>=2 && directory.binding_count && directory.event_count);
        bool parameter=false,returned=false,inner=false,cell=false,begin=false,end=false;
        bool leave=false,stopped=false,continued=false,post_cleanup=false;
        const XrXirFunction *function=&module->functions[f];
        for (uint32_t b=0;b<directory.binding_count;++b) {
            XirLexicalBinding binding=directory.bindings[b];
            CHECK(binding.key.function==f && binding.scope<directory.scope_count && binding.order==b);
            parameter|=binding.role==XIR_LEXICAL_PARAMETER_READ && binding.value<function->parameter_count;
            cell|=binding.rebindable && binding.value>=function->parameter_count &&
                binding.value-function->parameter_count<function->instruction_count &&
                function->instructions[binding.value-function->parameter_count].op==XR_XIR_CELL_NEW;
        }
        for (uint32_t e=0;e<directory.event_count;++e) {
            XirLexicalEvent event=directory.events[e];
            returned|=event.kind==XIR_LEXICAL_RETURN && event.target==UINT32_MAX &&
                function->instructions[event.instruction].op==XR_XIR_RETURN;
            inner|=event.kind==XIR_LEXICAL_ENTER && event.scope>1;
            begin|=event.kind==XIR_LEXICAL_TEMP_BEGIN && event.statement;
            end|=event.kind==XIR_LEXICAL_TEMP_END && event.statement;
            leave|=event.kind==XIR_LEXICAL_LEAVE;
            stopped|=event.kind==XIR_LEXICAL_BREAK;
            continued|=event.kind==XIR_LEXICAL_CONTINUE;
            if (event.kind==XIR_LEXICAL_LEAVE && event.position==XIR_LEXICAL_BLOCK_ENTRY) {
                for (uint32_t i=0;i<function->instruction_count;++i) {
                    const XrXirInstruction *op=&function->instructions[i];
                    post_cleanup|=op->op==XR_XIR_CLEANUP_LEAVE && op->targets[0]==event.block;
                }
            }
        }
        CHECK(parameter && returned && begin && end);
        if (n<3) CHECK(inner && leave);
        if (n<2) CHECK(cell);
        if (n==1) CHECK(stopped && continued);
        if (n==2) CHECK(post_cleanup);
        if (n==5) {
            uint32_t ignored=UINT32_MAX,bound=UINT32_MAX;
            for (uint32_t i=0;i<function->instruction_count;++i) {
                if (function->instructions[i].op!=XR_XIR_CALL) continue;
                if (ignored==UINT32_MAX) ignored=function->parameter_count+i;
                else if (bound==UINT32_MAX) bound=function->parameter_count+i;
            }
            CHECK(ignored!=UINT32_MAX && bound!=UINT32_MAX);
            bool named=false,discarded=false;
            for (uint32_t b=0;b<directory.binding_count;++b) {
                named|=directory.bindings[b].value==bound;
                discarded|=directory.bindings[b].value==ignored;
            }
            CHECK(named && !discarded);
        }
        if (n==6) {
            bool cleanup_boundary=false;
            for (uint32_t e=0;e<directory.event_count;++e) {
                const XirLexicalEvent *event=&directory.events[e];
                if (event->kind!=XIR_LEXICAL_LEAVE && event->kind!=XIR_LEXICAL_TEMP_END) continue;
                if (event->position==XIR_LEXICAL_AFTER)
                    cleanup_boundary|=function->instructions[event->instruction].op==XR_XIR_VIEW_CLEANUP;
                if (event->position==XIR_LEXICAL_BEFORE && event->instruction)
                    cleanup_boundary|=function->instructions[event->instruction-1].op==XR_XIR_VIEW_CLEANUP;
            }
            CHECK(cleanup_boundary);
        }
        if (n==7) {
            bool external=false,handler=false;
            for (uint32_t e=0;e<directory.event_count;++e) {
                const XirLexicalEvent *event=&directory.events[e];
                if (event->kind!=XIR_LEXICAL_PANIC) continue;
                external|=event->target==UINT32_MAX;
                handler|=event->target!=UINT32_MAX && event->position==XIR_LEXICAL_PANIC_EDGE;
            }
            CHECK(external && handler);
        }
    }
    XirLexicalFunction unchanged={.scope_count=UINT32_MAX};
    CHECK(xr_xir_compile_lexical_function(artifact,root_find(module,"lexicalPlanned"),&unchanged)==XR_XIR_UNRESOLVED);
    CHECK(unchanged.scope_count==UINT32_MAX && !unchanged.scopes);
    uint32_t unit=root_find(module,"lexicalUnit");
    XirLexicalFunction directory={0};
    CHECK(xr_xir_compile_lexical_function(artifact,unit,&directory)==XR_XIR_OK);
    bool local=false,returned=false;
    for (uint32_t b=0;b<directory.binding_count;++b) {
        const XirLexicalBinding *binding=&directory.bindings[b];
        local|=binding->role==XIR_LEXICAL_LOCAL && binding->value==UINT32_MAX && binding->physical==XR_XIR_UNIT;
    }
    for (uint32_t e=0;e<directory.event_count;++e)
        returned|=directory.events[e].kind==XIR_LEXICAL_RETURN && directory.events[e].value==UINT32_MAX;
    CHECK(local && returned);
    const char *modes[]={"lexicalBorrow","lexicalMove"};
    const XirLexicalBindingRole roles[]={XIR_LEXICAL_PARAMETER_REF,XIR_LEXICAL_PARAMETER_MOVE};
    for (unsigned n=0;n<2;++n) {
        uint32_t f=root_find(module,modes[n]);
        CHECK(xr_xir_compile_lexical_function(artifact,f,&directory)==XR_XIR_OK);
        bool found=false;
        for (uint32_t b=0;b<directory.binding_count;++b) {
            const XirLexicalBinding *binding=&directory.bindings[b];
            bool direct=binding->role==roles[n] && binding->value==0 &&
                binding->physical==module->functions[f].parameters[0];
            bool moved_cell=false;
            if (n==1 && binding->role==XIR_LEXICAL_LOCAL && binding->mode==XR_PARAM_MOVE &&
                binding->value>=module->functions[f].parameter_count) {
                uint32_t i=binding->value-module->functions[f].parameter_count;
                moved_cell=i<module->functions[f].instruction_count &&
                    module->functions[f].instructions[i].op==XR_XIR_CELL_NEW_TAKE &&
                    module->functions[f].instructions[i].args[0]==0 &&
                    binding->logical==module->functions[f].parameters[0];
            }
            found|=direct || moved_cell;
        }
        if (!found) {
            for (uint32_t b=0;b<directory.binding_count;++b) fprintf(stderr,
                "lexical mode %s b%u role%u mode%u value%u logical%u physical%u param%u\n",modes[n],b,
                directory.bindings[b].role,directory.bindings[b].mode,directory.bindings[b].value,
                directory.bindings[b].logical,directory.bindings[b].physical,module->functions[f].parameters[0]);
        }
        CHECK(found);
    }
}

static void lexical_source_ownership(void) {
    EffectMark baseline=effect_mark();
    XrXirArtifact *source=root_source("lexical_scopes.xr",XR_XIR_OK);
    lexical_source_expectations(source);
    XrXirCheckedPacket packet={0};
    XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_checked_write(source,&packet,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"lexical cold write status%u f%u b%u i%u shape%u\n",
        status,diagnostic.function,diagnostic.block,diagnostic.instruction,
        xir_lexical_verify(&effect_context,&source->module,&source->module.provenance->lexical));
    CHECK(status==XR_XIR_OK);
    lexical_packet_boundaries(&packet,&source->module.provenance->lexical);
    XrXirArtifact *specialized=NULL;
    CHECK(xr_xir_compile_specialize(source,&specialized,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);source=NULL;
    const XrXirModule *closed=xr_xir_compile_artifact_module(specialized);
    uint32_t found=0;
    for (uint32_t f=0;f<closed->function_count;++f) {
        XirLexicalFunction directory={0};
        if (xr_xir_compile_lexical_function(specialized,f,&directory)!=XR_XIR_OK) continue;
        for (uint32_t b=0;b<directory.binding_count;++b) {
            CHECK(directory.bindings[b].key.function==f);
            CHECK((uint32_t)directory.bindings[b].physical<XR_XIR_TYPE_PARAMETER_BASE ||
                (uint32_t)directory.bindings[b].physical>=XR_XIR_TYPE_PARAMETER_LIMIT);
        }
        ++found;
    }
    CHECK(found>=8);
    xr_xir_compile_artifact_free(specialized);
    CHECK(xr_xir_compile_checked_read(&effect_context,packet.bytes,packet.length,&source,NULL)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    lexical_source_expectations(source);
    EffectMark retained=effect_mark();
    const XirLexicalProgram *input=&source->module.provenance->lexical;
    XirLexicalProgram copy={0};
    attempts=0;injected=false;
    CHECK(xir_lexical_copy(&effect_context,input,&copy,NULL)==XR_XIR_OK);
    size_t sites=attempts;
    xir_lexical_free(&copy);
    effect_mark_check(retained);
    for (size_t point=0;point<sites;++point) {
        attempts=0;injected=false;fail_at=point;
        CHECK(xir_lexical_copy(&effect_context,input,&copy,NULL)==XR_XIR_OUT_OF_MEMORY && injected && !copy.functions);
        fail_at=SIZE_MAX;
        effect_mark_check(retained);
    }
    xr_xir_compile_artifact_free(source);
    effect_mark_check(baseline);
    printf("Lexical Source same-block scopes, loop exits, cleanup order, specialized/cold owner and %zu FI PASS\n",sites);
}

/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_signal_frontier_cases.h - Real parent frames at idle driver frontiers
 *
 * KEY CONCEPT:
 *   These checks observe eligibility, never register or execute a signal
 *   handler. Both engines run the same sealed parent and normal loan endings.
 */
#ifndef HOST_OS_SIGNAL_FRONTIER_CASES_H
#define HOST_OS_SIGNAL_FRONTIER_CASES_H
typedef struct HostSignalFrontier {
 unsigned views,loans,no_suspend,parent_safe,parent_after_view,cleanup,fault,busy,observing;
 unsigned initializing_safe,initializing_view,initializing_after_view,unpublished_reads;
 XrXirCallObserver observer;
 XirCallNativeObserver native_observer;
 XrXirExecutorBinding held_binding;
 uint64_t held_frame;
 bool hold_case,hold_seen,parent_native;
} HostSignalFrontier;
static HostSignalFrontier *host_signal_counts;
static XrXirCallStatus host_signal_throw(void *owner,const XirCallNativeThrowEvent *event,bool *pause){
 HostSignalFrontier *counts=owner;CHECK(counts->hold_case&&!counts->hold_seen);
 counts->held_binding=event->frame.binding;counts->held_frame=event->frame.frame;
 *pause=true;return XR_XIR_CALL_READY;
}
static void host_signal_observer(void *owner,const XrXirCallEvent *event){
 HostSignalFrontier *counts=owner;CHECK(host_active&&event->instance==host_active);
 bool eligible=true;CHECK(xir_call_signal_frontier(host_active->call,&eligible)==XR_XIR_CALL_BUSY&&eligible);
 ++counts->observing;
}
static void host_signal_provider_probe(void){
 CHECK(host_signal_counts&&host_active);bool eligible=true;
 CHECK(xir_call_signal_frontier(host_active->call,&eligible)==XR_XIR_CALL_BUSY&&eligible);
 ++host_signal_counts->busy;
 if(host_active->state==XR_XIR_INSTANCE_INITIALIZING){
  const XrXirDeclarations *d=host_active->program->declarations;
  CHECK(d->slot_count==1&&d->slots[0].module==host_active->current_module&&!host_active->published[0]);
  XrXirValue unpublished={0};
  CHECK(xr_xir_instance_slot_read(host_active->call->active_view,0,&unpublished)==XR_XIR_CALL_BAD_STATE&&
   !unpublished.type&&!unpublished.reserved&&!unpublished.payload);
  ++host_signal_counts->unpublished_reads;
 }
 CHECK(xr_xir_call_observer_bind(host_active->call->active_view,&host_signal_counts->observer)==XR_XIR_CALL_READY);
 if(host_signal_counts->hold_case)
  CHECK(xir_call_native_observer_bind(host_active->call->active_view,&host_signal_counts->native_observer)==XR_XIR_CALL_READY);
}
static bool host_signal_observe(XrXirInstance *instance,uint32_t parent,HostSignalFrontier *counts){
 XrXirCall *call=instance->call;unsigned char saved[sizeof(*call)];memcpy(saved,call,sizeof(saved));
 CallFrame *frames[16];unsigned char states[16][sizeof(CallFrame)];unsigned n=0;
 bool view=false,loan=false,protected=false,parent_live=false,cleanup=false,fault=false;
 for(CallFrame *frame=call->top;frame;frame=frame->parent){
  CHECK(n<16);frames[n]=frame;memcpy(states[n++],frame,sizeof(*frame));
  uint32_t entry=(uint32_t)(frame->entry-frame->metadata->entries);
  if(frame->metadata->cell_context==instance->program&&entry<instance->program->entry_count){
   protected|=(instance->program->declarations->functions[entry].promises&XR_XIR_FUNCTION_NO_SUSPEND)!=0;
   parent_live|=entry==parent&&!frame->exiting;
   if(entry==parent&&frame->code_kind==XR_XIR_CALL_CODE_NATIVE)counts->parent_native=true;
  }
  for(CallViewBinding *binding=frame->views;binding;binding=binding->next)
   view|=xr_xir_view_scope_active(&binding->pin);
  for(uint32_t i=0;i<frame->loan_count;++i)loan|=frame->loans[i].loan.cell!=NULL;
  cleanup|=frame->exiting||frame->in_cleanup||frame->cleanup_call;
  fault|=xr_xir_call_panic_status(frame->pending.status)||xr_xir_call_panic_status(frame->inbox.status);
 }
 XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(instance->domain);size_t attempts=runtime_attempts;
 bool eligible=false;CHECK(xir_call_signal_frontier(call,&eligible)==XR_XIR_CALL_READY);
 CHECK(!memcmp(saved,call,sizeof(saved))&&runtime_attempts==attempts);
 for(unsigned i=0;i<n;++i)CHECK(!memcmp(states[i],frames[i],sizeof(CallFrame)));
 XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(instance->domain);
 CHECK(before.work==after.work&&before.requested_bytes==after.requested_bytes&&
  before.requested_call_bytes==after.requested_call_bytes&&before.call_live==after.call_live&&before.metadata_live==after.metadata_live);
 if(view){CHECK(!eligible);++counts->views;}if(loan){CHECK(!eligible);++counts->loans;}
 if(protected){CHECK(!eligible);++counts->no_suspend;}if(cleanup){CHECK(!eligible);++counts->cleanup;}
 if(fault){CHECK(!eligible);++counts->fault;}
 if(eligible){CHECK(call->top);if(parent_live){++counts->parent_safe;if(counts->views)++counts->parent_after_view;}}
 if(instance->state==XR_XIR_INSTANCE_INITIALIZING&&parent_live){
  uint32_t module=instance->program->declarations->functions[parent].module;
  CHECK(instance->current_module==module&&!instance->ready[module]&&!instance->published[0]);
  if(view)++counts->initializing_view;
  if(eligible){++counts->initializing_safe;if(counts->initializing_view)++counts->initializing_after_view;}
 }
 return view;
}
static XrXirCallStatus host_signal_poll(XrXirInstance *instance,uint32_t parent,HostSignalFrontier *counts,bool cancel){
 bool cancelled=false;
 for(unsigned n=0;n<100000;++n){
  bool view=host_signal_observe(instance,parent,counts);
  if(instance->budget.native_hold){
   XrXirCall *held=instance->budget.native_hold;CHECK(counts->hold_case&&!counts->hold_seen&&held!=instance->call);
   bool eligible=true;CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY&&!eligible);
   CHECK(xr_xir_call_executor_member(held,&counts->held_binding));counts->hold_seen=true;
   CHECK(xir_call_native_throw_continue(held,&counts->held_binding,counts->held_frame)==XR_XIR_CALL_READY);
  }
  if(cancel&&view&&!cancelled){
   CHECK(xr_xir_call_request_cancel(instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;
   bool eligible=true;CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY&&!eligible);
  }
  XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
  if(status!=XR_XIR_CALL_READY){if(cancel)CHECK(cancelled);return status;}
 }
 CHECK(false);return XR_XIR_CALL_LIMIT;
}
static void host_signal_initializing_execute(XrXirProgram *program,const uint32_t ids[5]){
 size_t live=runtime_live,bytes=runtime_bytes;
 for(unsigned cancel=0;cancel<2;++cancel){
  XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
  XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
  CHECK(instance->state==XR_XIR_INSTANCE_NEW);host_active=instance;host_mode=HOST_NORMAL;
  HostSignalFrontier counts={0};counts.observer=(XrXirCallObserver){&counts,host_signal_observer};
  host_signal_counts=&counts;host_count_probe=host_signal_provider_probe;
  CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY&&instance->state==XR_XIR_INSTANCE_INITIALIZING);
  XrXirCallStatus status=host_signal_poll(instance,ids[1],&counts,cancel!=0);
  CHECK(counts.initializing_safe&&counts.initializing_view&&counts.unpublished_reads);
  if(cancel){
   CHECK(status==XR_XIR_CALL_CANCELLED&&instance->state==XR_XIR_INSTANCE_FAILED&&!instance->published[0]);
   CHECK(xr_xir_instance_start(instance,ids[4],NULL,0)==XR_XIR_CALL_CANCELLED);
  }else{
   CHECK(status==XR_XIR_CALL_RETURNED&&instance->state==XR_XIR_INSTANCE_READY&&counts.initializing_after_view);
   XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&
    value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);
   CHECK(instance->published[0]&&instance->ready[program->declarations->root_module]);
   value=host_invoke(instance,ids[4],NULL,0);CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
  }
  bool eligible=true;CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY&&!eligible);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
  host_count_probe=NULL;host_signal_counts=NULL;CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
}
static void host_signal_frontier_execute(XrXirProgram *program,const uint32_t ids[5]){
 host_signal_initializing_execute(program,ids);
 size_t live=runtime_live,bytes=runtime_bytes;
 for(unsigned mode=0;mode<4;++mode){
  XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;
  HostSignalFrontier counts={0};counts.observer=(XrXirCallObserver){&counts,host_signal_observer};
  counts.hold_case=mode==3;counts.native_observer=(XirCallNativeObserver){&counts,host_signal_throw};
  host_signal_counts=&counts;host_count_probe=host_signal_provider_probe;
  uint32_t parent=ids[mode==3?3:mode==2?2:1];
  CHECK(xr_xir_instance_start(instance,parent,NULL,0)==XR_XIR_CALL_READY);
  if(mode==0){
   /* An identical public entry table is not an owned Root or sealed promise. */
   XrXirCallAccounting accounting={0};XrXirCallConfig config=instance->call->config;config.accounting=&accounting;
   XrXirCall *raw=NULL;CHECK(xr_xir_call_new(&config,ids[1],NULL,0,&raw)==XR_XIR_CALL_READY);
   bool eligible=true;CHECK(xir_call_signal_frontier(raw,&eligible)==XR_XIR_CALL_READY&&!eligible);
   CHECK(xr_xir_call_free(raw)==XR_XIR_CALL_READY&&!accounting.live_bytes);
  }
  host_count_probe=NULL;
  XrXirInstance *other=host_instance(program,ids[0]);bool no_suspend=true;
  host_count_probe=host_signal_provider_probe;
  const CallFrame *top=instance->call->top;
  CHECK(!xir_instance_signal_frame(other,instance->call,&instance->call->config.admission,
   top->metadata,(uint32_t)(top->entry-top->metadata->entries),&no_suspend)&&no_suspend);
  CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);
  bool untouched=true;CHECK(xir_call_signal_frontier(NULL,&untouched)==XR_XIR_CALL_BAD_ARGUMENT&&untouched);
  CHECK(xir_call_signal_frontier(instance->call,NULL)==XR_XIR_CALL_BAD_ARGUMENT);
  XrXirCallStatus status=host_signal_poll(instance,parent,&counts,mode==1);
  CHECK(status==((mode==0||mode==3)?XR_XIR_CALL_RETURNED:mode==1?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_ASSERTION));
  if(mode==0||mode==3){
   if(mode==0)CHECK(counts.views&&counts.loans&&counts.no_suspend&&counts.parent_safe&&counts.parent_after_view&&
    counts.cleanup&&counts.busy&&counts.observing);
   else CHECK(counts.hold_seen==counts.parent_native);
   XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&
    value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
  }else if(mode==1)CHECK(counts.views&&counts.cleanup);
  else CHECK(counts.fault);
  bool eligible=true;CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY&&!eligible);
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
  host_count_probe=NULL;host_signal_counts=NULL;
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
}
#endif // HOST_OS_SIGNAL_FRONTIER_CASES_H

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
 XrXirCallObserver observer;
} HostSignalFrontier;
static HostSignalFrontier *host_signal_counts;
static void host_signal_observer(void *owner,const XrXirCallEvent *event){
 HostSignalFrontier *counts=owner;CHECK(host_active&&event->instance==host_active);
 bool eligible=true;CHECK(xir_call_signal_frontier(host_active->call,&eligible)==XR_XIR_CALL_BUSY&&eligible);
 ++counts->observing;
}
static void host_signal_provider_probe(void){
 CHECK(host_signal_counts&&host_active);bool eligible=true;
 CHECK(xir_call_signal_frontier(host_active->call,&eligible)==XR_XIR_CALL_BUSY&&eligible);
 ++host_signal_counts->busy;
 CHECK(xr_xir_call_observer_bind(host_active->call->active_view,&host_signal_counts->observer)==XR_XIR_CALL_READY);
}
static void *host_signal_foreign(void *pointer){
 bool eligible=true;
 CHECK(xir_call_signal_frontier(pointer,&eligible)==XR_XIR_CALL_BAD_ARGUMENT&&eligible);
 return NULL;
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
 return view;
}
static XrXirCallStatus host_signal_poll(XrXirInstance *instance,uint32_t parent,HostSignalFrontier *counts,bool cancel){
 bool cancelled=false;
 for(unsigned n=0;n<100000;++n){
  bool view=host_signal_observe(instance,parent,counts);
  if(cancel&&view&&!cancelled){
   CHECK(xr_xir_call_request_cancel(instance->call)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;
   bool eligible=true;CHECK(xir_call_signal_frontier(instance->call,&eligible)==XR_XIR_CALL_READY&&!eligible);
  }
  XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
  if(status!=XR_XIR_CALL_READY){if(cancel)CHECK(cancelled);return status;}
 }
 CHECK(false);return XR_XIR_CALL_LIMIT;
}
static void host_signal_frontier_execute(XrXirProgram *program,const uint32_t ids[3]){
 size_t live=runtime_live,bytes=runtime_bytes;
 for(unsigned mode=0;mode<3;++mode){
  XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;host_mode=HOST_NORMAL;
  HostSignalFrontier counts={0};counts.observer=(XrXirCallObserver){&counts,host_signal_observer};
  host_signal_counts=&counts;host_count_probe=host_signal_provider_probe;
  CHECK(xr_xir_instance_start(instance,ids[mode==2?2:1],NULL,0)==XR_XIR_CALL_READY);
  if(mode==0){
   /* An identical public entry table is not an owned Root or sealed promise. */
   XrXirCallAccounting accounting={0};XrXirCallConfig config=instance->call->config;config.accounting=&accounting;
   XrXirCall *raw=NULL;CHECK(xr_xir_call_new(&config,ids[1],NULL,0,&raw)==XR_XIR_CALL_READY);
   bool eligible=true;CHECK(xir_call_signal_frontier(raw,&eligible)==XR_XIR_CALL_READY&&!eligible);
   CHECK(xr_xir_call_free(raw)==XR_XIR_CALL_READY&&!accounting.live_bytes);
  }
  xr_thread_t foreign;CHECK(xr_thread_create(&foreign,host_signal_foreign,instance->call));
  void *returned=NULL;CHECK(!xr_thread_join(foreign,&returned)&&!returned);
  bool untouched=true;CHECK(xir_call_signal_frontier(NULL,&untouched)==XR_XIR_CALL_BAD_ARGUMENT&&untouched);
  CHECK(xir_call_signal_frontier(instance->call,NULL)==XR_XIR_CALL_BAD_ARGUMENT);
  XrXirCallStatus status=host_signal_poll(instance,ids[mode==2?2:1],&counts,mode==1);
  CHECK(status==(mode==0?XR_XIR_CALL_RETURNED:mode==1?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_ASSERTION));
  if(mode==0){
   CHECK(counts.views&&counts.loans&&counts.no_suspend&&counts.parent_safe&&counts.parent_after_view&&
    counts.cleanup&&counts.busy&&counts.observing);
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

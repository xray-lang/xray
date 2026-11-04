/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_exec_shell.inc.c - Shell candidates owned by ordinary process retirement
 */
static XrXirCallStatus instance_process_spawn_shell(XrXirCallView *view,XrXirInstance *instance,XirProcessFrame *frame) {
    XrXirInstance *root=instance_root(instance);XrOsProcessRequest request={0};XrOsProcessText command={0};
    XrXirCallStatus status=instance_process_text(view,view->arguments,&command);bool valid=true;
    XirPipeQuery current={view,instance->domain,status};
    for(uint32_t p=1;p<3&&status==XR_XIR_CALL_READY;++p) {
        uint32_t end=0;XirPipeOwner *pipe=instance_pipe_lookup(root,view->arguments[p].payload,&end,&current);
        status=current.status;if(status!=XR_XIR_CALL_READY)break;
        if(!pipe||end!=XR_OS_PIPE_WRITE){valid=false;continue;}
        status=instance_pipe_status(xr_os_io_pipe_borrow(&pipe->owner,XR_OS_PIPE_WRITE,request.streams+p));
        request.redirected[p]=true;
    }
    if(status==XR_XIR_CALL_READY&&valid) {
        XirProcessOwner *node=instance_metadata_allocate(root->domain,sizeof(*node),&status);
        if(node) {
            *node=(XirProcessOwner){.next=root->process_owners,.generation=instance->generation_id,.references=1};
            root->process_owners=node;frame->process=node;
            status=instance_process_token(&node->token);
            XrOsIoPolicy memory={root->domain,instance_pipe_allocate,instance_pipe_free};
            XrOsProcessQuery query={&current,instance_pipe_current};current.status=status;
            if(status==XR_XIR_CALL_READY)status=instance_pipe_status(
                xr_os_io_process_prepare_shell(&memory,&query,&request,&command,&node->owner,&frame->created));
            if(!instance_pipe_current(&current))status=current.status;
        }
    }
    /* Publication and start still use the Process frame's final commit. A
     * later Source wrapper failure leaves its already-published Root duty. */
    if(status==XR_XIR_CALL_READY)*frame->frame=(XrXirValue){XR_XIR_I64,0,frame->created?frame->process->token:0};
    return status;
}

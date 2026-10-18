/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace.c - Workspace background indexing and file scanning
 */

#include "xlsp_workspace.h"
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "xlsp_utils.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../../base/xmalloc.h"
#include "../../os/os_time.h"
#include "xlsp_source_index_store.h"
#include "xlsp_index_scan.h"

// lsp_log declared in xlsp_server.h (included via xlsp_workspace.h)
// ============================================================================
// Background Indexing
// ============================================================================

#include "xlsp_async.h"
#include "xlsp_index_pool.h"
#include "../../os/os_dir.h"
#include "../../api/xsource_compile.h"

static bool workspace_index_pool_ready(XrLspServer *server) {
    if(server->source_revision==UINT64_MAX){xlsp_index_record_failure(server,XR_XIR_BUDGET);return false;}
    if(server->index_pool)return true;
    if(!server->index_resources||!server->source_index){xlsp_index_record_failure(server,XR_XIR_BUDGET);return false;}
    unsigned workers=xr_os_cpu_count();workers=workers>1?workers-1:1;if(workers<2)workers=2;if(workers>64)workers=64;
    XlspIndexPoolOptions options={server->index_resources,50u*1024u*1024u,workers};
    XrXirStatus status=xlsp_index_pool_new(&options,&server->index_pool);
    if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Source index pool admission failed: %u",(unsigned)status);return false;}
    return true;
}

// Disk snapshots are display facts only; opening a file never grants types.
void xlsp_workspace_index_file(XrLspServer *server,const char *uri,const char *path) {
    (void)path;
    if(!server||!uri||!workspace_index_pool_ready(server))return;
    char *decoded=NULL,*canonical=NULL;XrXirStatus status=xlsp_source_uri_path(server->index_resources,uri,strlen(uri),&decoded);
    if(status==XR_XIR_OK)status=xlsp_index_file_uri(server->index_resources,decoded,&canonical);
    if(status==XR_XIR_OK)status=xlsp_source_index_store_remove(server->source_index,canonical);
    if(status==XR_XIR_OK)status=xlsp_index_pool_submit(server->index_pool,decoded,canonical,server->source_revision);
    xr_compile_resources_free(canonical);xr_compile_resources_free(decoded);
    if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Changed-file Source index admission failed: %u",(unsigned)status);}
}

// ============================================================================
// Multi-Isolate Parallel Indexing (New)
// ============================================================================

// Merge index results into workspace analyzer (called from main thread)
// This performs the actual symbol merging by re-analyzing files in the main thread
void xlsp_workspace_merge_index_results(XrLspServer *server, XrLspIndexResult *results) {
    if (!server || !results)
        return;

    // Check if indexing was cancelled
    if (server->index_cancelled) {
        lsp_log("[IndexPool] Indexing was cancelled, discarding results");
        // End progress with cancellation message
        if (server->index_progress_token) {
            xlsp_progress_end(server, server->index_progress_token, "Indexing cancelled");
            xr_free(server->index_progress_token);
            server->index_progress_token = NULL;
        }
        server->indexing_in_progress = false;
        return;
    }

    int merged_count = 0;
    int error_count = 0;
    int symbols_added = 0;

    for (XrLspIndexResult *result = results; result; result = result->next) {
        // Check for cancellation during merge
        if (server->index_cancelled) {
            lsp_log("[IndexPool] Indexing cancelled during merge");
            break;
        }

        if(atomic_load(&result->cancel_requested))continue;
        if(result->revision!=server->source_revision) {
            if(!xlsp_document_get(server,result->uri)) {
                XrXirStatus retry=server->source_revision==UINT64_MAX?XR_XIR_BUDGET:
                    xlsp_index_pool_submit(server->index_pool,result->path,result->uri,server->source_revision);
                if(retry!=XR_XIR_OK)xlsp_index_record_failure(server,retry);
            }
            continue;
        }
        if(result->status!=XR_XIR_OK||!result->index) {
            lsp_log("Source display indexing failed: %s status=%u",result->path?result->path:"",(unsigned)result->status);
            xlsp_index_record_failure(server,result->status==XR_XIR_OK?XR_XIR_BAD_STRUCTURE:result->status);
            ++error_count;continue;
        }
        XrLspDocument *open=xlsp_document_get(server,result->uri);
        if(open)continue;
        size_t indexed=0;xlsp_source_index_symbols(result->index,&indexed);
        XrXirStatus received=xlsp_source_index_store_replace(server->source_index,&result->index);
        if(received!=XR_XIR_OK){xlsp_index_record_failure(server,received);++error_count;lsp_log("Source index receiving failed: %u",(unsigned)received);continue;}
        if(indexed>(size_t)(INT_MAX-symbols_added)){++error_count;continue;}
        symbols_added+=(int)indexed;

        server->files_indexed++;
        merged_count++;
    }

    if (merged_count > 0 || error_count > 0) {
        lsp_log("[IndexPool] Merged %d files (%d errors, %d symbols), total: %d/%d", merged_count,
                error_count, symbols_added, server->files_indexed, server->files_total);

        // Update progress
        if (server->index_progress_token && server->files_total > 0) {
            int percentage=(int)((uint64_t)server->files_indexed*100u/(unsigned)server->files_total);
            char msg[128];
            snprintf(msg, sizeof(msg), "Indexed %d/%d files (%d symbols)", server->files_indexed,
                     server->files_total, symbols_added);
            xlsp_progress_report(server, server->index_progress_token, msg, percentage);
        }
    }

    // Check if indexing is complete
    if (server->index_pool && xlsp_index_pool_is_idle(server->index_pool)) {
        uint64_t submitted=0,completed=0;
        xlsp_index_pool_get_progress(server->index_pool,&submitted,&completed);

        if (submitted > 0 && submitted == completed) {
            server->indexing_in_progress = false;

            // Report the shallow index size: under Approach A closed files live
            // in the workspace symbol index, not the (open-files-only) analyzer.
            size_t total_symbols=xlsp_source_index_store_count(server->source_index);

            lsp_log("[IndexPool] Background indexing complete: %llu files, %zu indexed symbols",
                    (unsigned long long)completed,total_symbols);

            // End progress with success message
            if (server->index_progress_token) {
                char msg[128];
                if(server->index_failure!=XR_XIR_OK)snprintf(msg,sizeof(msg),"Source indexing failed (status=%u)",(unsigned)server->index_failure);
                else snprintf(msg,sizeof(msg),"Indexed %llu files (%zu symbols)",(unsigned long long)completed,total_symbols);
                xlsp_progress_end(server, server->index_progress_token, msg);
                xr_free(server->index_progress_token);
                server->index_progress_token = NULL;
            }
        }
    }
}

// Poll index pool and process results (call from main loop)
void xlsp_workspace_poll_index_results(XrLspServer *server) {
    if (!server || !server->index_pool)
        return;

    XrLspIndexResult *results = xlsp_index_pool_poll(server->index_pool);
    if (results) {
        xlsp_workspace_merge_index_results(server, results);
        xlsp_index_result_free_list(results);
    }
}

static bool workspace_ignore_index_file(const char *name,bool directory,void *context) {
    return xlsp_config_should_ignore(context,name,directory);
}
void xlsp_workspace_start_background_index_roots(XrLspServer *server,const char **roots,int root_count) {
    if(!server||!roots||root_count<=0||!workspace_index_pool_ready(server))return;
    XlspIndexPaths paths={0};XlspIndexScanOptions options={server->index_resources,roots,(size_t)root_count,workspace_ignore_index_file,&server->config};
    XrXirStatus status=xlsp_index_scan(&options,&paths);
    int prior=server->indexing_in_progress?server->files_total:0;
    if(status==XR_XIR_OK&&(prior<0||paths.count>(size_t)(INT_MAX-prior)))status=XR_XIR_BUDGET;
    if(status==XR_XIR_OK)status=xlsp_index_pool_submit_batch(server->index_pool,paths.values,paths.count,server->source_revision);
    if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Source workspace indexing admission failed: %u; no partial scan/batch published",(unsigned)status);xlsp_index_paths_free(&paths);return;}
    if(!server->indexing_in_progress){server->files_indexed=0;server->files_total=0;}
    server->files_total+=(int)paths.count;server->indexing_in_progress=paths.count!=0||server->indexing_in_progress;
    server->index_cancelled=false;
    if(paths.count&&!server->index_progress_token)server->index_progress_token=xlsp_progress_begin(server,"Indexing Workspace","Reading Source syntax",true);
    xlsp_index_paths_free(&paths);
}
void xlsp_workspace_start_background_index(XrLspServer *server,const char *root_path) {
    if(!root_path)return;const char *roots[]={root_path};xlsp_workspace_start_background_index_roots(server,roots,1);
}

// Purge all analyzer/cache state for files under a path prefix
void xlsp_workspace_purge_prefix(XrLspServer *server, const char *path_prefix) {
    if (!server || !path_prefix)
        return;

    size_t prefix_len = strlen(path_prefix);
    if (prefix_len == 0)
        return;
    if(server->source_revision==UINT64_MAX){xlsp_index_record_failure(server,XR_XIR_BUDGET);return;}
    ++server->source_revision;server->index_cancelled=true;xlsp_index_pool_cancel_all(server->index_pool);

    // Invalidate exports cache for files under this prefix
    xlsp_exports_cache_remove_prefix(server, path_prefix);

    // Drop shallow workspace-symbol entries for files under this folder.
    if (server->source_index) {
        char *uri=NULL;XrXirStatus status=xlsp_index_file_uri(server->index_resources,path_prefix,&uri);
        if(status==XR_XIR_OK)status=xlsp_source_index_store_remove_prefix(server->source_index,uri);
        xr_compile_resources_free(uri);if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Source index folder removal failed: %u",(unsigned)status);}
    }

    lsp_log("[Workspace] Purged state for prefix: %s (len=%zu)", path_prefix, prefix_len);
}

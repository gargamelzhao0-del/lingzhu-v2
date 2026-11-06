#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "conversation/conversation_metrics.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static nuwax_conversation_metrics_t *s_conversation_runtime_metrics;
static bool s_conversation_runtime_metrics_ready;
static bool fail_allocation=true;
static unsigned allocations;
static void *heap_caps_calloc(size_t n,size_t size,int caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(n==1 && size==sizeof(nuwax_conversation_metrics_t));
    if(fail_allocation)return NULL;
    allocations++;return calloc(n,size);
}
#include "conversation/runtime_metrics_storage.inc"
int main(void)
{
    assert(!metrics_runtime_prepare(1,1000) && !s_conversation_runtime_metrics);
    fail_allocation=false;
    s_conversation_runtime_metrics_ready=metrics_runtime_prepare(2,2000);
    assert(s_conversation_runtime_metrics_ready && allocations==1);
    nuwax_conversation_metrics_t *saved=s_conversation_runtime_metrics;
    assert(nuwax_conversation_metrics_record_duration(saved,NUWAX_CONVERSATION_LATENCY_DSP_PROCESS,150));
    nuwax_conversation_metrics_snapshot_t snapshot;
    assert(nuwax_conversation_metrics_snapshot(saved,3000,&snapshot));
    assert(snapshot.round_id==2 && snapshot.latency[NUWAX_CONVERSATION_LATENCY_DSP_PROCESS].count==1);
    fail_allocation=true; // Subsequent rounds must reuse storage even under allocation pressure.
    s_conversation_runtime_metrics_ready=metrics_runtime_prepare(3,4000);
    assert(s_conversation_runtime_metrics_ready && s_conversation_runtime_metrics==saved && allocations==1);
    assert(nuwax_conversation_metrics_snapshot(saved,5000,&snapshot));
    assert(snapshot.round_id==3 && snapshot.latency[NUWAX_CONVERSATION_LATENCY_DSP_PROCESS].count==0);
    free(saved);
    puts("PASS: optional metrics PSRAM-only allocation, failure/retry, real histogram/snapshot and allocation-free round reuse");
}

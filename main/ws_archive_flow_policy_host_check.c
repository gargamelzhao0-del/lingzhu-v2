#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "ws_archive_flow_policy.h"
int main(void)
{
    const uint32_t windows[]={46080,131072,196608};
    const size_t internals[]={4096,8192,12287,12288,16384,20479,20480,65536};
    const size_t free_bytes[]={0,256*1024,768*1024,800*1024,1024*1024,2*1024*1024,SIZE_MAX};
    unsigned cases=0;
    for(unsigned w=0;w<3;w++)for(unsigned i=0;i<8;i++)for(unsigned f=0;f<7;f++)
    for(uint32_t q=0;q<=windows[w];q+=1024){
        uint32_t limit=nuwax_ws_archive_queue_limit(windows[w],internals[i],free_bytes[f],768*1024,q);
        assert(limit<=windows[w]-8192);
        if(free_bytes[f]<=768*1024)assert(limit<=q);
        if(internals[i]<12288)assert(limit<=16384);
        else if(internals[i]<20480)assert(limit<=32768);
        if(nuwax_ws_archive_queue_admits(limit,q,8256))assert(limit>=q+8256);
        assert(!nuwax_ws_archive_queue_admits(limit,q,SIZE_MAX));
        ++cases;
    }
    assert(!nuwax_ws_archive_queue_admits(1,UINT32_MAX,1));
    nuwax_ws_archive_deadline_t s={0};
    assert(nuwax_ws_archive_frame_deadline(&s,0,250000,8250000,false,false,false)==250000);
    assert(nuwax_ws_archive_frame_deadline(&s,50000,250000,8250000,true,false,true)==2550000);
    assert(nuwax_ws_archive_frame_deadline(&s,100000,250000,8250000,true,true,true)==2600000);
    assert(nuwax_ws_archive_frame_deadline(&s,2000000,250000,8250000,true,true,true)==2600000);
    assert(nuwax_ws_archive_frame_deadline(&s,2200000,250000,8250000,true,false,true)==2600000);
    s=(nuwax_ws_archive_deadline_t){0};
    for(int64_t now=0;now<9000000;now+=50000)
        assert(nuwax_ws_archive_frame_deadline(&s,now,250000,8250000,true,false,true)<=8250000);
    s=(nuwax_ws_archive_deadline_t){0};
    assert(nuwax_ws_archive_frame_deadline(&s,100000,250000,300000,true,false,true)==300000);
    const int64_t later=(int64_t)UINT32_MAX*1000+5000000;
    s=(nuwax_ws_archive_deadline_t){0};
    assert(nuwax_ws_archive_frame_deadline(&s,later,later+250000,later+8250000,true,false,true)==later+2500000);
    printf("PASS %u queue-memory cases: control/capture reserves, fallback, no unsigned underflow; progress/cancel/heartbeat/absolute and 32-bit-clock boundaries\n",cases);
    return 0;
}

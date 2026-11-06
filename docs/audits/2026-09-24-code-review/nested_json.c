
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "cJSON.h"
int main(void) {
    char p[2048]="{\"type\":\"pong\",\"extra\":";
    size_t n=strlen(p);
    for(int i=0;i<256;i++)p[n++]='[';
    p[n++]='0';
    for(int i=0;i<256;i++)p[n++]=']';
    p[n++]='}';p[n]=0;
    cJSON *r=cJSON_ParseWithLength(p,n);
    assert(r&&cJSON_IsObject(r));
    printf("nested arrays=256 payload_bytes=%zu parser_accepted=1\n",n);
    cJSON_Delete(r);
}

#include <assert.h>
#include <stdio.h>
#include "archive_ghash.h"

static uint32_t rng = 0x201500U;
static uint8_t random_byte(void)
{ rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5; return (uint8_t)rng; }

/* Independent bit-serial GF reference, no lookup table and no production
 * byte/word helper. The high input bit multiplies by H itself. */
static void reference_block(uint8_t hash[16], const uint8_t h[16], const uint8_t block[16])
{
    uint8_t x[16],v[16],z[16]={0};memcpy(v,h,16);
    for(unsigned i=0;i<16;i++)x[i]=hash[i]^block[i];
    for(unsigned bit=0;bit<128;bit++){
        if((x[bit/8]>>(7-(bit%8)))&1U)for(unsigned i=0;i<16;i++)z[i]^=v[i];
        const uint8_t lsb=v[15]&1U;
        for(int i=15;i>=0;i--)v[i]=(uint8_t)((v[i]>>1)|(i?v[i-1]<<7:0));
        if(lsb)v[0]^=0xe1U;
    }
    memcpy(hash,z,16);
}
static void reference(uint8_t hash[16],const uint8_t h[16],const uint8_t *data,size_t bytes)
{
    while(bytes>=16){reference_block(hash,h,data);data+=16;bytes-=16;}
    if(bytes){uint8_t last[16]={0};memcpy(last,data,bytes);reference_block(hash,h,last);}
}
int main(void)
{
    static archive_ghash_table_t table;
    static const size_t sizes[]={0,1,7,15,16,17,31,32,63,128,511,1024,4097};
    uint8_t data[4101],h[16],initial[16],a[18],b[16],fragmented[16];unsigned checks=0;
    for(unsigned key=0;key<64;key++){
        for(unsigned i=0;i<16;i++){h[i]=key?random_byte():0;initial[i]=random_byte();}
        for(unsigned i=0;i<sizeof(data);i++)data[i]=random_byte();
        archive_ghash_table_prepare(table,h);
        for(unsigned align=0;align<4;align++)for(unsigned k=0;k<sizeof(sizes)/sizeof(sizes[0]);k++){
            const size_t n=sizes[k];a[0]=0x93;a[17]=0x67;memcpy(a+1,initial,16);memcpy(b,initial,16);
            archive_ghash_update(table,a+1,data+align,n);reference(b,h,data+align,n);
            assert(a[0]==0x93&&a[17]==0x67&&!memcmp(a+1,b,16));
            memcpy(fragmented,initial,16);const size_t prefix=n>64?64U:n&~(size_t)15U;
            archive_ghash_update(table,fragmented,data+align,prefix);
            archive_ghash_update(table,fragmented,data+align+prefix,n-prefix);
            assert(!memcmp(fragmented,b,16));checks++;
        }
    }
    assert(!archive_ghash_worthwhile(0,0)&&!archive_ghash_worthwhile(100,0));
    assert(archive_ghash_worthwhile(1000,850)&&!archive_ghash_worthwhile(1000,851));
    assert(!archive_ghash_worthwhile(1000,1000)&&!archive_ghash_worthwhile(1000,2000));
    printf("PASS byte GHASH: %u independent bit-serial comparisons, zero/random H, unaligned input/hash, tails, fragmented aligned updates, guards and 15%% calibration gate\n",checks);
    return 0;
}


#include <stdio.h>
#include <math.h>
#include "board/shake_motion_policy.h"
int main(void){int totals[2]={0},ok[2]={0};
for(int drop=0;drop<2;drop++)for(int speed=0;speed<3;speed++)for(int phase=0;phase<4;phase++)for(int offset=0;offset<5;offset++){
 nuwax_shake_motion_t s={0};int hits=0;float freq[]={1.25,2,3};
 for(unsigned t=0;t<=4000;t+=50){
  if(drop && (t/50)%5==offset){nuwax_shake_motion_invalidate(&s);continue;}
  float a[]={650*sinf(6.2831853f*freq[speed]*t/1000.0f+phase*1.5707963f),0,1000};
  hits+=nuwax_shake_motion_observe(&s,t,a,0,4000);
 }
 totals[drop]++;if(hits)ok[drop]++;else printf("miss drop=%d hz=%.2f phase=%d skip_phase=%d\n",drop,freq[speed],phase,offset);
}
printf("SUMMARY baseline=%d/%d periodic20percent=%d/%d\n",ok[0],totals[0],ok[1],totals[1]);}

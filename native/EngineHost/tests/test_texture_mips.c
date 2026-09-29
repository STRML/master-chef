#include "../texture_mips.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
static void reference(const uint8_t *s,unsigned w,unsigned h,size_t p,uint8_t *o){
 unsigned nw=w>1?w/2:1,nh=h>1?h/2:1;
 for(unsigned y=0;y<nh;y++)for(unsigned x=0;x<nw;x++)for(unsigned k=0;k<4;k++){
  unsigned sum=0,n=0;
  for(unsigned sy=y*h/nh;sy<(y+1)*h/nh;sy++)for(unsigned sx=x*w/nw;sx<(x+1)*w/nw;sx++){sum+=s[sy*p+sx*4+k];n++;}
  o[(y*nw+x)*4+k]=(sum+n/2)/n;
 }
}
int main(void){
 unsigned seed=1;
 for(unsigned h=1;h<=65;h++)for(unsigned w=1;w<=65;w++){
  size_t pitch=w*4+7,n=(w>1?w/2:1)*(h>1?h/2:1)*4;
  uint8_t *s=malloc(h*pitch),*a=malloc(n),*b=malloc(n);
  for(size_t i=0;i<h*pitch;i++){seed=1664525u*seed+1013904223u;s[i]=seed>>24;}
  reference(s,w,h,pitch,a);halo_texture_mip(s,w,h,pitch,b);assert(!memcmp(a,b,n));free(s);free(a);free(b);
 }
 unsigned w=2048,h=2048;size_t n=(size_t)w*h*4;uint8_t *s=malloc(n),*a=malloc(n/4),*b=malloc(n/4);
 for(size_t i=0;i<n;i++)s[i]=(i*31+i/17)&255;
 clock_t t=clock();for(int i=0;i<6;i++)reference(s,w,h,w*4,a);double old=(double)(clock()-t)/CLOCKS_PER_SEC;
 t=clock();for(int i=0;i<6;i++)halo_texture_mip(s,w,h,w*4,b);double now=(double)(clock()-t)/CLOCKS_PER_SEC;
 assert(!memcmp(a,b,n/4));printf("PASS 4225 dimensions/padded rows + 2048 image exact; reference %.2f ms optimized %.2f ms for six levels\n",old*1000,now*1000);free(s);free(a);free(b);
}

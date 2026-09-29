#include "../panorama_budget.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 HaloPanoramaBudget b;halo_panorama_budget_init(&b);
 HaloPanoramaView v[]={{0,-1.04719755f,0,0},{1,0,0,0},{2,1.04719755f,0,0},{5,0,1.57079633f,0},{6,0,-1.57079633f,0},{8,3.14159265f,0,0},{4,0,0,1}};
 uint32_t centre=1u<<1;
 assert(halo_panorama_budget_motion_fill(&b,v,7,0,centre,0,0,1.f/30)==centre); /* no timings */
 b.pass_ema=.005f;b.busy_ema=.010f;b.ppf_ema=1;
 uint32_t filled=halo_panorama_budget_motion_fill(&b,v,7,0,centre,0,0,1.f/30);
 assert(filled==(centre|1u|(1u<<2))); /* both visible joins fresh */
 b.pass_ema=.018f;b.busy_ema=.026f;
 assert(halo_panorama_budget_motion_fill(&b,v,7,0,centre,0,0,1.f/30)==centre); /* costly scene keeps floor */
 b.pass_ema=.005f;b.busy_ema=.023f;
 filled=halo_panorama_budget_motion_fill(&b,v,7,0,centre,0,0,1.f/30);assert(__builtin_popcount(filled)==2); /* one fits */
 b.pass_ema=NAN;assert(halo_panorama_budget_motion_fill(&b,v,7,0,centre,0,0,1.f/30)==centre);
 /* Looking ahead still exposes the cap/ring joins. Both fit after the three
  * forward bearings, but the rear and unused right eye remain untouched. */
 uint32_t front=centre|1u|(1u<<2);
 b.pass_ema=.003f;b.busy_ema=.009f;b.ppf_ema=3;
 assert(halo_panorama_budget_motion_fill(&b,v,7,0,front,0,0,1.f/30)==(front|(1u<<5)|(1u<<6)));
 b.busy_ema=.026f;b.last_drawn[5]=100;b.last_drawn[6]=90;
 assert(halo_panorama_budget_motion_fill(&b,v,7,0,front,0,0,1.f/30)==(front|(1u<<6))); /* only oldest cap fits */
 b.busy_ema=.009f;
 filled=halo_panorama_budget_motion_fill(&b,v,7,0,front,0,-1.f,1.f/30);
 assert((filled&(1u<<6)) && !(filled&(1u<<5))); /* no opposite cap */
 halo_panorama_budget_init(&b);
 halo_panorama_budget_plan(&b,v,7,0,1,0,0);
 assert(b.yaw_rate==0 && b.pitch_rate==0);
 halo_panorama_budget_plan(&b,v,7,0,2,.02f,.04f);
 assert(fabsf(b.yaw_rate-.02f)<1e-6f && fabsf(b.pitch_rate-.04f)<1e-6f);
 puts("PASS motion coverage: visible joins use measured headroom; expensive scenes and invalid timing stay bounded");
}

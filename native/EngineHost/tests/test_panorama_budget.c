/* clang -O2 native/EngineHost/tests/test_panorama_budget.c -lm -o /tmp/halo-budget && /tmp/halo-budget */
#include "../panorama_budget.h"
#include <assert.h>
#include <stdio.h>
static const HaloPanoramaView views[]={
    {0,-M_PI/3,0,0},{2,M_PI/3,0,0},{5,0,M_PI/2,0},{6,0,-M_PI/2,0},
    {7,2*M_PI/3,0,0},{8,M_PI,0,0},{9,-2*M_PI/3,0,0},{4,0,0,1},{1,0,0,0}
};
enum { COUNT = 9, CENTRE_PAIR = (1u<<1)|(1u<<4) };
static uint32_t plan(HaloPanoramaBudget *b, int stereo, uint64_t frame, float yaw, float pitch) {
    uint32_t mask = halo_panorama_budget_plan(b, views, COUNT, stereo, frame, yaw, pitch);
    halo_panorama_budget_drawn(b, mask, frame);
    return mask;
}
int main(void) {
    /* Saturation must not accumulate phantom extras. Forward stereo has
     * four mandatory passes and only five optional ones. Previously the
     * controller banked eighteen half-passes, then spent two seconds
     * shedding credits before the real nine-pass workload changed. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.extra_half=18;
      assert(plan(&b,1,1,0,0)==HALO_PANORAMA_STEREO_MASK);
      assert(b.extra_half==10);
      for(int f=2;f<=120;f++) {
          halo_panorama_budget_observe(&b,0.020f,1.f/30.f,18);
          assert(plan(&b,1,f,0,0)==HALO_PANORAMA_STEREO_MASK);
          assert(b.extra_half==10);
      } }
    /* A cheap corridor followed by a costly room: shed optional work
     * within the first 0.8 s while keeping both eyes and both visible
     * neighbours. This is a deterministic cost model, not headset FPS. */
    for(int heavy=0;heavy<2;heavy++) {
      HaloPanoramaBudget b; halo_panorama_budget_init(&b);
      float elapsed=0.f,first_fit=-1.f; unsigned missed=0;
      for(unsigned f=1;f<=900;f++) {
          float each=f<=600?0.0018f:(heavy?0.0045f:0.0035f);
          assert(b.tier==0);
          uint32_t mask=plan(&b,1,f,0,0);
          assert((mask&(CENTRE_PAIR|1u|4u))==(CENTRE_PAIR|1u|4u));
          unsigned count=__builtin_popcount(mask);
          float work=0.0045f+count*each,period=fmaxf(work,1.f/30.f);
          halo_panorama_budget_note_passes(&b,each*count,count);
          halo_panorama_budget_observe_timed(&b,work,period,1.f/30.f,18);
          if(f>600) {
              if(work>1.f/30.f)missed++;
              else if(first_fit<0.f)first_fit=elapsed;
              elapsed+=period;
          }
      }
      assert(first_fit>=0.f && first_fit<0.8f);
      assert(missed<20);
    }
    /* Stationary gaze anywhere: valid layers only, the centre pair always
     * together, the gaze bearing every frame, no starvation at the floor. */
    for(int stereo=0;stereo<=1;stereo++)for(int y=-12;y<=12;y++)for(int p=-6;p<=6;p++) {
        HaloPanoramaBudget b; halo_panorama_budget_init(&b);
        uint32_t seen=0,valid=stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
        float yaw=y*M_PI/12, pitch=p*M_PI/12;
        for(uint64_t frame=1;frame<=32;frame++) {
            uint32_t mask=plan(&b,stereo,frame,yaw,pitch);
            assert((mask&~valid)==0);
            assert(mask&(1u<<1));
            if(stereo)assert((mask&CENTRE_PAIR)==CENTRE_PAIR); else assert(!(mask&(1u<<4)));
            /* the best-scoring view is in the mask */
            float best=-2; int best_layer=-1;
            for(int n=0;n<COUNT;n++){ if(!stereo&&views[n].layer==4)continue;
                float s=halo_panorama_budget_score(&views[n],yaw,pitch); if(s>best){best=s;best_layer=views[n].layer;} }
            assert(mask&(1u<<best_layer));
            seen|=mask;
        }
        assert(seen==valid);
    }
    /* Floor budget forward in stereo: the pair and both neighbours every
     * frame plus one extra every other frame, 540 passes per 120 frames;
     * the three behind and the two caps each come round within twelve. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); unsigned passes=0; uint64_t last[HALO_PANORAMA_LAYERS]={0};
      for(uint64_t frame=1;frame<=120;frame++) {
          uint32_t mask=plan(&b,1,frame,0,0); passes+=__builtin_popcount(mask);
          assert((mask&CENTRE_PAIR)==CENTRE_PAIR); assert(mask&1u); assert(mask&4u);
          for(int l=0;l<HALO_PANORAMA_LAYERS;l++) if(mask&(1u<<l)) last[l]=frame;
          if(frame>=12) for(int l=0;l<HALO_PANORAMA_LAYERS;l++)
              if((HALO_PANORAMA_STEREO_MASK>>l)&1u) assert(frame-last[l]<=12);
      }
      assert(passes==540); }
    /* Fast frames grow the budget to the ceiling, slow frames shrink it back
     * to the floor; each step waits its eight frames. The target is a
     * ceiling: just under it holds, just over it gives way. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); const float target=1.f/26.f;
      for(int i=0;i<8*14;i++) halo_panorama_budget_observe(&b,0.020f,target,14);
      assert(b.extra_half==14);
      uint32_t mask=plan(&b,1,1,0,0); assert(mask==HALO_PANORAMA_STEREO_MASK);
      for(int i=0;i<8*14;i++) halo_panorama_budget_observe(&b,0.060f,target,14);
      assert(b.extra_half==1);
      halo_panorama_budget_observe(&b,5.f,target,14); /* a load is not a frame */
      assert(b.extra_half==1);
      for(int i=0;i<8;i++) halo_panorama_budget_observe(&b,0.038f,target,14); /* just under: hold */
      assert(b.extra_half==1);
      b.extra_half=5;
      for(int i=0;i<8;i++) halo_panorama_budget_observe(&b,0.040f,target,14); /* just over: give way */
      assert(b.extra_half>=3 && b.extra_half<=4); }
    /* A head turning right lands on the right bearing before the gaze does. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b);
      plan(&b,1,1,0.f,0);
      uint32_t turning=plan(&b,1,2,0.2f,0);
      assert(turning&(1u<<2));
      /* Eleven degrees off centre with no motion is still the centre. */
      float best=-2; int best_layer=-1;
      for(int n=0;n<COUNT;n++){ float sc=halo_panorama_budget_score(&views[n],0.2f,0); if(sc>best){best=sc;best_layer=views[n].layer;} }
      assert(best_layer==1||best_layer==4); }
    /* With one extra pass a frame the five bearings behind and above take
     * turns: each is drawn once in five frames, none twice. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.extra_half=2;
      uint32_t seen=0; const uint32_t front=CENTRE_PAIR|1u|4u;
      for(uint64_t frame=1;frame<=5;frame++) {
          uint32_t mask=plan(&b,1,frame,0,0)&~front;
          assert(__builtin_popcount(mask)==1); assert(!(seen&mask)); seen|=mask;
      }
      assert(seen==(HALO_PANORAMA_STEREO_MASK&~front)); }
    /* The heavy tiers. At the floor and still over the target the budget
     * climbs a tier every ninety frames, to three; each tier draws fewer
     * passes forward; from tier two the centre is mono; well under the
     * target it comes back down, and only then do the extras grow again. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); const float target=1.f/27.f;
      unsigned passes_at[4]={0}; int mono_at[4]={0};
      for(int tier=0;tier<=3;tier++){
          assert(b.tier==(unsigned)tier);
          int mono=halo_panorama_budget_mono(&b); mono_at[tier]=mono;
          for(uint64_t frame=1;frame<=8;frame++){ uint32_t mask=plan(&b,!mono,1000+tier*100+frame,0,0); passes_at[tier]+=__builtin_popcount(mask);
              assert(mask&(1u<<1)); if(mono)assert(!(mask&(1u<<4))); else assert((mask&CENTRE_PAIR)==CENTRE_PAIR); }
          /* Over the target but under the 1.5x jump: one tier per up-dwell. */
          for(int i=0;i<(int)((HALO_PANORAMA_TIER_UP_SECONDS+HALO_PANORAMA_BUDGET_ADJUST_SECONDS)*27.f)+2;i++) halo_panorama_budget_observe(&b,0.045f,target,14);
      }
      assert(b.tier==3); assert(b.extra_half==1);
      assert(passes_at[0]>passes_at[1] && passes_at[1]>passes_at[2] && passes_at[2]>passes_at[3]);
      assert(!mono_at[0]&&!mono_at[1]&&mono_at[2]&&mono_at[3]);
      assert(passes_at[3]<=8*2); /* tier 3 forward: the centre and a neighbour every other frame, an extra every eighth */
      /* still over the target at tier 3: stays there */
      for(int i=0;i<400;i++) halo_panorama_budget_observe(&b,0.060f,target,14);
      assert(b.tier==3);
      /* well under: down one tier at a time, each step a dwell apart, the
       * extras held at 1 until tier 0 and growing only then */
      { unsigned previous=3; int last_change=-1, changes=0;
        for(int i=0;i<600;i++){
            halo_panorama_budget_observe(&b,0.015f,target,14);
            if(b.tier!=previous){ assert(b.tier==previous-1);
                                  if(last_change>=0) assert((i-last_change)*target>=(previous==2?HALO_PANORAMA_STEREO_DWELL_SECONDS:HALO_PANORAMA_TIER_DOWN_SECONDS)-0.05f);
                                  previous=b.tier; last_change=i; changes++; }
            if(b.tier>0) assert(b.extra_half==1);
        }
        assert(changes==3 && b.tier==0 && b.extra_half>1); } }
    /* At tier 1 the neighbours take turns as the mandatory set (an extra
     * may still pick the other): over two frames both are drawn, and over
     * twenty frames at tier 3 the forward set averages under two passes. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=1; uint32_t seen=0;
      for(uint64_t frame=1;frame<=2;frame++){ uint32_t mask=plan(&b,1,frame,0,0); assert(mask&((frame&1u)?1u:4u)); seen|=mask; }
      assert((seen&(1u|4u))==(1u|4u));
      halo_panorama_budget_init(&b); b.tier=3; unsigned passes=0;
      for(uint64_t frame=1;frame<=40;frame++) passes+=__builtin_popcount(plan(&b,0,frame,0,0));
      assert(passes<=40*2 && passes>=40); }
    /* Tier three must spread the same thirteen passes across eight forward
     * frames. Placing its distant pass on an even frame also drew a neighbour,
     * making every eighth frame three passes long for no freshness benefit. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3;
      unsigned passes=0, maximum=0;
      for(uint64_t frame=1;frame<=64;frame++) {
          unsigned count=__builtin_popcount(plan(&b,0,frame,0,0));
          passes+=count; if(count>maximum)maximum=count;
      }
      assert(passes==104); assert(maximum==2); }
    /* A cap toward the gaze must not inherit the old neighbour/extra spike. */
    for(int direction=-1;direction<=1;direction+=2) {
        HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3;
        unsigned passes=0;
        for(uint64_t frame=1;frame<=72;frame++) {
            unsigned count=__builtin_popcount(plan(&b,0,frame,0,direction*0.6f));
            if(frame>8) { assert(count<=2); passes+=count; }
        }
        assert(passes==120);
    }
    /* Isolate the distant refresh from caps/neighbours: changing pitch after
     * the cycle chose its slot must not cancel or duplicate that refresh. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3;
      const HaloPanoramaView pair[]={{1,0,0,0},{8,M_PI,0,0}};
      const uint32_t centre=1u<<1, both=centre|(1u<<8);
      assert(halo_panorama_budget_plan(&b,pair,2,0,8,0,0.6f)==centre);
      assert(halo_panorama_budget_plan(&b,pair,2,0,9,0,-0.6f)==centre);
      assert(halo_panorama_budget_plan(&b,pair,2,0,10,0,-0.6f)==centre);
      assert(halo_panorama_budget_plan(&b,pair,2,0,11,0,-0.6f)==both);
      assert(halo_panorama_budget_plan(&b,pair,2,0,16,0,-0.6f)==centre);
      assert(halo_panorama_budget_plan(&b,pair,2,0,17,0,0.6f)==both);
      assert(halo_panorama_budget_plan(&b,pair,2,0,19,0,0.6f)==centre); }
    /* Every heavy tier and stationary gaze must keep every active bearing
     * fresh. This also exercises pitched caps, off-axis views, and transitions
     * back to stereo instead of testing only the forward floor schedule. */
    for(unsigned tier=0;tier<=3;tier++)for(int y=-12;y<=12;y++)for(int p=-6;p<=6;p++) {
        HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=tier;
        int stereo=!halo_panorama_budget_mono(&b);
        uint32_t valid=stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
        for(uint64_t frame=1;frame<=256;frame++) {
            uint32_t mask=plan(&b,stereo,frame,y*M_PI/12,p*M_PI/12);
            assert((mask&~valid)==0); assert(mask&(1u<<1));
            if(frame>=64)for(int l=0;l<HALO_PANORAMA_LAYERS;l++)
                if(valid&(1u<<l))assert(frame-b.last_drawn[l]<=64);
        }
        for(int next=3;next>=0;next--) {
            b.tier=(unsigned)next; stereo=!halo_panorama_budget_mono(&b);
            uint32_t mask=plan(&b,stereo,257+(3-next),y*M_PI/12,p*M_PI/12);
            assert(mask&(1u<<1));
            assert(!!(mask&(1u<<4))==stereo);
        }
    }
    /* Coming back down needs room for one more pass, not a busy time the
     * 30 fps cap never shows. With passes timed at 5 ms a 26 ms frame has it:
     * the budget steps down to stereo and on, the step to stereo after its
     * longer dwell. At 12 ms a pass the same frame has no room and stays. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3; const float target=1.f/27.f;
      unsigned previous=3; int last_change=0;
      for(int i=0;i<1200 && b.tier>0;i++){
          halo_panorama_budget_note_passes(&b,0.005f*1.6f,2);
          halo_panorama_budget_observe(&b,0.026f,target,14);
          if(b.tier!=previous){ assert(b.tier==previous-1);
              if(previous==2) assert((i-last_change)*target>=HALO_PANORAMA_STEREO_DWELL_SECONDS-0.05f);
              previous=b.tier; last_change=i; }
      }
      assert(b.tier==0);
      halo_panorama_budget_init(&b); b.tier=3;
      for(int i=0;i<1200;i++){ halo_panorama_budget_note_passes(&b,0.012f,1); halo_panorama_budget_observe(&b,0.026f,target,14); }
      assert(b.tier==3); }
    /* Well over the target the extras halve instead of stepping down one by one. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.extra_half=12; const float target=1.f/27.f;
      for(int i=0;i<8;i++) halo_panorama_budget_observe(&b,1.3f*target,target,14);
      assert(b.extra_half==6); }
    /* The seam in front of the eyes: looking 26 degrees right of the centre
     * bearing draws the right neighbour every frame at tiers one and two and
     * every other frame at tier three, 26 degrees left the left one; looking
     * straight ahead adds nothing. */
    for(unsigned tier=1;tier<=3;tier++) for(int side=-1;side<=1;side+=2) {
        HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=tier;
        int stereo=!halo_panorama_budget_mono(&b);
        uint32_t neighbour=side>0?(1u<<2):(1u<<0);
        for(uint64_t frame=1;frame<=64;frame++)
            if(tier<3 || !(frame&1u)) assert(plan(&b,stereo,frame,side*0.45f,0)&neighbour);
            else plan(&b,stereo,frame,side*0.45f,0);
        halo_panorama_budget_init(&b); b.tier=tier; unsigned straight=0;
        for(uint64_t frame=1;frame<=64;frame++) if(plan(&b,stereo,frame,0,0)&neighbour) straight++;
        assert(straight<64);
    }
    /* Entering a level resets the budget to a heavy start: the menu's tier 0
     * and full extras must not come along, nor its cheap pass timings. The
     * next level starts where the last one settled. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=0; b.extra_half=18; b.pass_ema=0.0009f; b.busy_ema=0.008f;
      halo_panorama_budget_scene_entry(&b);
      assert(b.tier==HALO_PANORAMA_TIER_MAX && b.extra_half==1 && b.pass_ema==0.f && b.busy_ema==0.f);
      b.tier=2; halo_panorama_budget_note_level(&b); b.tier=0; b.extra_half=12;
      halo_panorama_budget_scene_entry(&b); assert(b.tier==2 && b.extra_half==1);
      b.tier=1; halo_panorama_budget_note_level(&b); halo_panorama_budget_scene_entry(&b); assert(b.tier==2); }
    /* Waits are seconds of measured frame period: at 11 fps the step down
     * still takes about three seconds, not ninety frames (eight seconds). */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3; const float target=1.f/30.f, period=1.f/11.f;
      int frames=0; while(b.tier==3 && frames<400){ halo_panorama_budget_observe_timed(&b,0.012f,period,target,14); frames++; }
      float seconds=frames*period; assert(b.tier==2 && seconds>=HALO_PANORAMA_TIER_DOWN_SECONDS-0.1f && seconds<=HALO_PANORAMA_TIER_DOWN_SECONDS+1.0f); }
    /* Far over the target, the budget jumps straight to the tier whose floor
     * fits instead of a tier per dwell. */
    { HaloPanoramaBudget b; halo_panorama_budget_init(&b); const float target=1.f/30.f;
      halo_panorama_budget_note_passes(&b,4.5f*0.010f,5);   /* 5 passes a frame, 9 ms each */
      for(int i=0;i<12;i++){ halo_panorama_budget_note_passes(&b,0.045f,5); halo_panorama_budget_observe_timed(&b,0.080f,0.080f,target,14); }
      assert(b.tier==HALO_PANORAMA_TIER_MAX); }
    /* Tier three with a join in view never stacks more than two passes. */
    for(int side=-1;side<=1;side+=2){ HaloPanoramaBudget b; halo_panorama_budget_init(&b); b.tier=3; unsigned maximum=0;
      for(uint64_t frame=1;frame<=64;frame++){ unsigned c=__builtin_popcount(plan(&b,0,frame,side*0.45f,0)); if(c>maximum)maximum=c; }
      assert(maximum<=2); }
    puts("panorama budget: validity, centre pair, gaze, neighbours, floor 540/120, no starvation, adaptation, prediction, fairness, heavy tiers, balanced cadence, all-tier starvation and transitions, cost-model relief, halving, gaze seam, scene entry, timed waits, overload jump, no stacked passes PASS");
    return 0;
}

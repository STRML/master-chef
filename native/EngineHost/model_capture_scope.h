#ifndef HALO_MODEL_CAPTURE_SCOPE_H
#define HALO_MODEL_CAPTURE_SCOPE_H
#include "model_capture_boundary.h"
#define HOST_MODEL_SCOPE_CAP 32u
typedef struct {
    uint32_t object;unsigned depth,overflow,entries,exits,deferred_matches,deferred_other;
    int active,overflow_previous,previous[HOST_MODEL_SCOPE_CAP];
} HostModelCaptureScope;
static inline int host_model_capture_eligible(const HostModelCaptureBoundary *b){
    return b->kind==HOST_MODEL_NORMAL&&b->caller==0x0050F049u&&!(b->flags&10u)&&b->type==0&&b->object!=UINT32_MAX&&b->nodes;
}
static inline int host_model_capture_scope_begin(HostModelCaptureScope *s,const HostModelCaptureBoundary *b){
    if(s->depth==HOST_MODEL_SCOPE_CAP){if(!s->overflow)s->overflow_previous=s->active;s->overflow++;s->active=0;return -1;}
    s->previous[s->depth]=s->active;s->depth++;s->entries++;
    int deferred=b->kind==HOST_MODEL_DEFERRED;
    s->active=!s->overflow&&b->object==s->object&&(deferred?!(b->flags&0x82u):host_model_capture_eligible(b));
    if(deferred){if(s->active)s->deferred_matches++;else s->deferred_other++;}
    return (int)s->depth;
}
static inline int host_model_capture_scope_end(HostModelCaptureScope *s,int token){
    if(token==-1&&s->overflow){s->overflow--;if(!s->overflow)s->active=s->overflow_previous;return 1;}
    if(token<=0||(unsigned)token!=s->depth){s->active=0;return 0;}
    s->depth--;s->exits++;s->active=!s->overflow&&s->previous[s->depth];return 1;
}
#endif

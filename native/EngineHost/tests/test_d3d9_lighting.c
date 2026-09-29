/* CPU lighting fixture. Include the translation unit so this checks the exact
 * fixed-function implementation used by the guest D3D9 bridge. */
#include "../d3d9.c"
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}

#ifdef D3D9_LIGHTING_BENCH
#include <time.h>
#endif

static int channel(uint32_t c,int shift){return (int)((c>>shift)&255u);}
static uint32_t reference_light_vertex(const float world[16],const float position[3],const float normal[3],uint32_t diffuse_color,uint32_t specular_color){
    if(!draw_state.rs[137])return diffuse_color;
    float c1[4],c2[4];unpack_color(diffuse_color,c1);unpack_color(specular_color,c2);
    const float *md=material_source(145,current_material.diffuse,c1,c2),*ma=material_source(147,current_material.ambient,c1,c2),*me=material_source(148,current_material.emissive,c1,c2);
    float ambient[4];unpack_color(draw_state.rs[139],ambient);float color[3]={me[0]+ma[0]*ambient[0],me[1]+ma[1]*ambient[1],me[2]+ma[2]*ambient[2]};
    float wp[3],wn[3];transform_position(world,position,wp);transform_normal(world,normal,wn);if(draw_state.rs[143])vec_normalize(wn);
    for(int i=0;i<HOST_D3D_MAX_LIGHTS;i++){HostD3DLightSlot*s=&light_slots[i];if(!s->defined||!s->enabled)continue;HostD3DLight*l=&s->light;float toward[3],atten=1.f,spot=1.f;
        if(l->type==3){toward[0]=-l->direction[0];toward[1]=-l->direction[1];toward[2]=-l->direction[2];if(!vec_normalize(toward))continue;}
        else {toward[0]=l->position[0]-wp[0];toward[1]=l->position[1]-wp[1];toward[2]=l->position[2]-wp[2];float d=vec_normalize(toward);if(!d||d>l->range)continue;float den=l->attenuation[0]+l->attenuation[1]*d+l->attenuation[2]*d*d;if(!(den>0.f))continue;atten=1.f/den;
            if(l->type==2){float dir[3]={l->direction[0],l->direction[1],l->direction[2]};if(!vec_normalize(dir))continue;float rho=-(toward[0]*dir[0]+toward[1]*dir[1]+toward[2]*dir[2]);float outer=cosf(l->phi*.5f),inner=cosf(l->theta*.5f);if(rho<=outer)spot=0.f;else if(rho<inner&&inner>outer)spot=powf((rho-outer)/(inner-outer),l->falloff);}
        }
        float ndotl=wn[0]*toward[0]+wn[1]*toward[1]+wn[2]*toward[2];if(ndotl<0.f)ndotl=0.f;if(ndotl>1.f)ndotl=1.f;
        for(int k=0;k<3;k++)color[k]+=atten*(ma[k]*l->ambient[k]+md[k]*l->diffuse[k]*ndotl*spot);
    }
    uint32_t a=(uint32_t)(light_clamp(md[3])*255.f+.5f),r=(uint32_t)(light_clamp(color[0])*255.f+.5f),g=(uint32_t)(light_clamp(color[1])*255.f+.5f),b=(uint32_t)(light_clamp(color[2])*255.f+.5f);
    return (a<<24)|(r<<16)|(g<<8)|b;
}
int main(void){
    _Static_assert(sizeof(HostD3DMaterial)==68,"D3DMATERIAL9 layout");
    _Static_assert(sizeof(HostD3DLight)==104,"D3DLIGHT9 layout");
    lighting_defaults();HostD3DMaterial roundtrip={0};roundtrip.diffuse[0]=.25f;roundtrip.ambient[1]=.5f;roundtrip.emissive[2]=.75f;roundtrip.power=12.f;assert(lighting_set_material(&roundtrip)==D3D_OK);assert(!memcmp(&current_material,&roundtrip,sizeof roundtrip));
    HostD3DLight stored={0};stored.type=1;stored.diffuse[0]=1;stored.position[2]=4;stored.range=10;stored.attenuation[0]=1;assert(lighting_set_light(7,&stored)==D3D_OK);assert(!memcmp(&light_slots[7].light,&stored,sizeof stored));assert(lighting_enable(7,1)==D3D_OK&&light_slots[7].enabled);
    lighting_defaults();for(int i=0;i<HOST_D3D_MAX_ACTIVE_LIGHTS;i++)assert(lighting_enable((uint32_t)i,1)==D3D_OK);assert(light_slots[0].defined&&light_slots[0].light.type==3&&light_slots[0].light.diffuse[0]==1.f);assert(lighting_enable(20,1)==D3DERR_INVALIDCALL);

    lighting_defaults();memset(&draw_state,0,sizeof draw_state);draw_state.rs[137]=1;draw_state.rs[143]=1;draw_state.rs[139]=0xFF202020u;
    current_material.diffuse[0]=.8f;current_material.diffuse[3]=1.f;current_material.ambient[0]=.5f;current_material.emissive[0]=.1f;
    HostD3DLight*l=&light_slots[0].light;default_light(l);l->direction[2]=-1.f;light_slots[0].defined=light_slots[0].enabled=1;
    float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},p[3]={0,0,0},n[3]={0,0,1};
    uint32_t c=d3d9_light_vertex(identity,p,n,0xFFFFFFFFu,0);assert(channel(c,24)==255);assert(channel(c,16)>=244&&channel(c,16)<=247);assert(channel(c,8)==0&&channel(c,0)==0);

    lighting_defaults();memset(&current_material,0,sizeof current_material);current_material.diffuse[0]=current_material.diffuse[1]=current_material.diffuse[2]=current_material.diffuse[3]=1.f;
    l=&light_slots[0].light;default_light(l);l->direction[0]=-.4472136f;l->direction[1]=-.8944272f;l->direction[2]=0;light_slots[0].defined=light_slots[0].enabled=1;
    float nonuniform[16]={2,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},diagonal[3]={.7071068f,.7071068f,0};
    c=d3d9_light_vertex(nonuniform,p,diagonal,0xFFFFFFFFu,0);assert(channel(c,16)>250&&channel(c,8)>250&&channel(c,0)>250);

    lighting_defaults();memset(&current_material,0,sizeof current_material);current_material.diffuse[2]=1.f;current_material.diffuse[3]=1.f;
    l=&light_slots[1].light;memset(l,0,sizeof *l);l->type=1;l->diffuse[2]=1.f;l->position[2]=10.f;l->range=20.f;l->attenuation[0]=2.f;light_slots[1].defined=light_slots[1].enabled=1;
    c=d3d9_light_vertex(identity,p,n,0xFFFFFFFFu,0);assert(channel(c,0)>=126&&channel(c,0)<=129);assert(channel(c,16)==0&&channel(c,8)==0);
    l->range=5.f;c=d3d9_light_vertex(identity,p,n,0xFFFFFFFFu,0);assert((c&0xFFFFFFu)==0);

    l->type=2;l->range=20.f;l->attenuation[0]=1.f;l->position[2]=10.f;l->direction[2]=-1.f;l->theta=.4f;l->phi=1.f;l->falloff=1.f;
    c=d3d9_light_vertex(identity,p,n,0xFFFFFFFFu,0);assert(channel(c,0)==255);
    float outside[3]={10,0,0};c=d3d9_light_vertex(identity,outside,n,0xFFFFFFFFu,0);assert((c&0xFFFFFFu)==0);

    /* Direct slot mutation remains supported by the wrapper fixture. Compare
     * prepared lighting with the prechange per-vertex loop across light kinds,
     * material sources, nonuniform transforms, and a singular transform. */
    lighting_defaults();memset(&current_material,0,sizeof current_material);current_material.diffuse[0]=.63f;current_material.diffuse[1]=.41f;current_material.diffuse[2]=.27f;current_material.diffuse[3]=.72f;
    current_material.ambient[0]=.11f;current_material.ambient[1]=.19f;current_material.ambient[2]=.07f;current_material.emissive[0]=.03f;current_material.emissive[1]=.02f;current_material.emissive[2]=.05f;
    draw_state.rs[141]=1;draw_state.rs[145]=1;draw_state.rs[147]=0;draw_state.rs[148]=2;draw_state.rs[143]=1;draw_state.rs[139]=0xFF304020u;
    HostD3DLight *d=&light_slots[2].light;default_light(d);d->direction[0]=-.3f;d->direction[1]=.4f;d->direction[2]=-.8f;light_slots[2].defined=light_slots[2].enabled=1;
    HostD3DLight *q=&light_slots[7].light;memset(q,0,sizeof *q);q->type=1;q->diffuse[0]=.8f;q->diffuse[1]=.2f;q->position[0]=1.5f;q->position[1]=-.5f;q->position[2]=3.f;q->range=9.f;q->attenuation[0]=.7f;q->attenuation[1]=.1f;q->attenuation[2]=.03f;light_slots[7].defined=light_slots[7].enabled=1;
    HostD3DLight *s=&light_slots[9].light;memset(s,0,sizeof *s);s->type=2;s->diffuse[1]=.9f;s->position[0]=-.5f;s->position[2]=4.f;s->direction[2]=-1.f;s->range=12.f;s->attenuation[0]=1.f;s->theta=.35f;s->phi=1.1f;s->falloff=2.f;light_slots[9].defined=light_slots[9].enabled=1;
    float varied[3][16]={{1,0,0,0,0,1,0,0,0,0,1,0,.25f,-.5f,1.f,1},{2,0,0,0,0,.5f,0,0,0,0,3,0,-1.f,2.f,-.25f,1},{0,0,0,0,0,0,0,0,0,0,0,0,2,3,4,1}};
    float positions[3][3]={{0,0,0},{.4f,-.7f,1.2f},{-2.f,1.f,5.f}},normals[3][3]={{0,0,1},{.3f,.4f,.5f},{-1.f,.2f,.1f}};
    for(int mi=0;mi<3;mi++)for(int vi=0;vi<3;vi++){HostD3DPreparedLighting prepared;d3d9_prepare_lighting(varied[mi],&prepared);assert(prepared.count==3&&prepared.lights[0].light.type==3&&prepared.lights[1].light.type==1&&prepared.lights[2].light.type==2);uint32_t input=0xA1B2C3D4u,spec=0x10203040u;uint32_t expected=reference_light_vertex(varied[mi],positions[vi],normals[vi],input,spec);uint32_t actual=d3d9_light_vertex_prepared(&prepared,positions[vi],normals[vi],input,spec);assert(actual==expected);assert(d3d9_light_vertex(varied[mi],positions[vi],normals[vi],input,spec)==expected);}
#ifdef D3D9_LIGHTING_BENCH
    {HostD3DPreparedLighting prepared;d3d9_prepare_lighting(varied[1],&prepared);volatile uint32_t sink=0;const int iterations=300000;clock_t begin=clock();for(int i=0;i<iterations;i++)sink^=reference_light_vertex(varied[1],positions[i%3],normals[i%3],0xA1B2C3D4u,0x10203040u);double reference_seconds=(double)(clock()-begin)/CLOCKS_PER_SEC;begin=clock();for(int i=0;i<iterations;i++)sink^=d3d9_light_vertex_prepared(&prepared,positions[i%3],normals[i%3],0xA1B2C3D4u,0x10203040u);double prepared_seconds=(double)(clock()-begin)/CLOCKS_PER_SEC;printf("d3d9 lighting synthetic: reference=%.6fs prepared=%.6fs speedup=%.2fx sink=%08x\n",reference_seconds,prepared_seconds,reference_seconds/prepared_seconds,(unsigned)sink);}
#endif
    lighting_defaults();for(int mi=0;mi<3;mi++){HostD3DPreparedLighting prepared;d3d9_prepare_lighting(varied[mi],&prepared);assert(prepared.count==0);assert(d3d9_light_vertex_prepared(&prepared,positions[0],normals[0],0xFFFFFFFFu,0)==reference_light_vertex(varied[mi],positions[0],normals[0],0xFFFFFFFFu,0));}
    puts("d3d9 fixed-function lighting tests passed");return 0;
}

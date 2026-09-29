/* Actual FindFirst/Next/Close shims with a small filesystem/guest stack fixture. */
#include "../shims_kernel32.c"
#include <assert.h>
uint8_t *engine_flat_base;
static uint32_t last_error;
static void *handles[16];
void host_set_last_error(uint32_t e){last_error=e;}
void host_log(const char *s,...){(void)s;}
void host_trace(const char *s,...){(void)s;}
const char *host_path(const char *path,char *out,size_t size){snprintf(out,size,"%s",path);return out;}
uint32_t host_handle_new(int kind,void *object){assert(kind==HANDLE_FIND);for(unsigned i=1;i<16;i++)if(!handles[i]){handles[i]=object;return i;}abort();}
void *host_handle_object(uint32_t h,int kind){assert(kind==HANDLE_FIND);return h<16?handles[h]:NULL;}
void host_handle_close(uint32_t h){assert(h<16);handles[h]=NULL;}
static uint32_t call(HostShim fn,const uint32_t *args,unsigned n){
    EngineCPU cpu={0};cpu.gpr[4]=0x1000;S32(0x1000,0x12345678);
    for(unsigned i=0;i<n;i++)S32(0x1004+i*4,args[i]);fn(&cpu);
    assert(cpu.pc==0x12345678&&cpu.gpr[4]==0x1004+n*4);return cpu.gpr[0];
}
static unsigned enumerate(const char *directory,const char *pattern,unsigned *mask){
    snprintf(GPTR(0x2000),1024,"%s/%s",directory,pattern);uint32_t args[]={0x2000,0x3000};
    uint32_t h=call(shim_FindFirstFileA,args,2);unsigned count=0;*mask=0;
    if(h==UINT32_MAX){assert(last_error==2);return 0;}
    do{
        const char *name=GSTR(0x3000+44);uint32_t attrs=G32(0x3000);
        if(!strcmp(name,"New001")){*mask|=1;assert(attrs&0x10);}
        else if(!strcmp(name,"Second.Profile")){*mask|=2;assert(attrs&0x10);}
        else if(!strcmp(name,"blam.sav")){*mask|=4;assert(!(attrs&0x10)&&G32(0x3000+32)==7);}
        else if(!strcmp(name,"readme")){*mask|=8;assert(!(attrs&0x10));}
        else assert(!"unexpected match");
        count++;args[0]=h;
    }while(call(shim_FindNextFileA,args,2));
    assert(last_error==18);assert(call(shim_FindClose,&h,1));assert(!handles[h]);return count;
}
int main(void){
    char directory[]="/private/tmp/halo-find-profiles-XXXXXX";assert(mkdtemp(directory));
    char path[1024];const char *names[]={"New001","Second.Profile","blam.sav","readme"};
    for(unsigned i=0;i<4;i++){snprintf(path,sizeof path,"%s/%s",directory,names[i]);if(i<2)assert(!mkdir(path,0700));else{FILE*f=fopen(path,"wb");assert(f);assert(fwrite("profile",1,7,f)==7);fclose(f);}}
    engine_flat_base=calloc(1,0x10000);assert(engine_flat_base);unsigned mask;
    assert(enumerate(directory,"*.*",&mask)==4&&mask==15);
    assert(enumerate(directory,"*",&mask)==4&&mask==15);
    assert(enumerate(directory,"*.sav",&mask)==1&&mask==4);
    assert(enumerate(directory,"New001",&mask)==1&&mask==1);
    assert(enumerate(directory,"absent.*",&mask)==0&&mask==0);
    for(unsigned i=0;i<4;i++){snprintf(path,sizeof path,"%s/%s",directory,names[i]);assert(!(i<2?rmdir(path):unlink(path)));}
    assert(!rmdir(directory));free(engine_flat_base);
    puts("PASS: Win32 *.* discovers extensionless profile directories; normal filters, metadata, iteration/end errors and handle cleanup remain valid.");
}

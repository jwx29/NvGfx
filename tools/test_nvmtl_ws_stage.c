#include "../plugin/nvmtl_ws_stage.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    size_t packed=0, bytes=0;
    assert(!nvmtl_ws_stage_plan(1920,1080,4,&packed,&bytes));
    assert(packed==7680 && bytes==8294400);
    assert(nvmtl_ws_stage_plan(SIZE_MAX,2,4,&packed,&bytes));
    assert(nvmtl_ws_stage_plan(8192,8192,4,&packed,&bytes));
    assert(nvmtl_ws_stage_plan(1,0,4,&packed,&bytes));
    void *ptrs[3]; int leases[3];
    for (unsigned i=0; i<3; ++i) {leases[i]=nvmtl_ws_stage_acquire(4096,&ptrs[i]); assert(leases[i]>=0);}
    void *extra=0;assert(nvmtl_ws_stage_acquire(4096,&extra)<0);
    for(unsigned i=0;i<3;++i)nvmtl_ws_stage_release(leases[i]);
    unsigned char src[3*9],dst[3*16];
    for(unsigned i=0;i<sizeof(src);++i)src[i]=(unsigned char)i;
    memset(dst,0xcc,sizeof(dst));
    assert(!nvmtl_ws_stage_copy(dst,16,sizeof(dst),src,9,3));
    for(int y=0;y<3;y++){for(int x=0;x<9;x++)assert(dst[y*16+x]==src[y*9+x]);
                        for(int x=9;x<16;x++)assert(dst[y*16+x]==0xcc);}
    assert(nvmtl_ws_stage_copy(dst,16,3*16-8,src,9,3));
    assert(nvmtl_ws_stage_copy(dst,8,sizeof(dst),src,9,3));
    memset(dst,0xcc,sizeof(dst));
    assert(!nvmtl_ws_stage_copy(dst,9,sizeof(dst),src,9,3));
    for(unsigned i=0;i<sizeof(src);++i)assert(src[i]==dst[i]);
    puts("PASS: stage geometry, bounded leases, contiguous/padded copies, and bounds");
}

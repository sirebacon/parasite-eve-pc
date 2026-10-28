/* Compile the actual backend with only SDL failure boundaries intercepted.
 * Normal calls still use the real hidden GPU device and resource lifetimes. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool fail_map, fail_wait, fail_submit, fail_acquire;
static unsigned map_calls;
static SDL_GPUCommandBuffer *test_acquire(SDL_GPUDevice *device) {
    if(fail_acquire)return NULL;
    return SDL_AcquireGPUCommandBuffer(device);
}
static void *test_map(SDL_GPUDevice *device,SDL_GPUTransferBuffer *buffer,bool cycle) {
    ++map_calls;
    if(fail_map)return NULL;
    return SDL_MapGPUTransferBuffer(device,buffer,cycle);
}
static bool test_wait(SDL_GPUDevice *device,bool all,SDL_GPUFence *const *fences,Uint32 count) {
    bool done=SDL_WaitForGPUFences(device,all,fences,count);
    return fail_wait?false:done;
}
static SDL_GPUFence *test_submit(SDL_GPUCommandBuffer *cmd) {
    if(fail_submit){SDL_CancelGPUCommandBuffer(cmd);return NULL;}
    return SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
}
#define SDL_AcquireGPUCommandBuffer test_acquire
#define SDL_MapGPUTransferBuffer test_map
#define SDL_WaitForGPUFences test_wait
#define SDL_SubmitGPUCommandBufferAndAcquireFence test_submit
#include "../third_party/psyz/psyz/src/platform/sdl3_gpu.c"
#undef SDL_AcquireGPUCommandBuffer
#undef SDL_MapGPUTransferBuffer
#undef SDL_WaitForGPUFences
#undef SDL_SubmitGPUCommandBufferAndAcquireFence
int main(int argc,char **argv) {
    assert(argc==2);
    if(!InitPlatform())return 77;
    Draw_ResetBuffer();
    if(!strcmp(argv[1],"--upload")){
        for(unsigned acquire=0;acquire<2;++acquire){
            assert(SubmitCmdAndWait());
            n_vertices=MAX_VERTEX_COUNT-4;vertex_cur=vertex_buf+n_vertices;
            fail_map=!acquire;fail_acquire=acquire;
            Draw_EnsureBufferWillNotOverflow(6,6);
            assert(!n_vertices && !n_indices && vertex_cur==vertex_buf && index_cur==index_buf);
            fail_map=fail_acquire=false;
        }
        puts("Failed upload cannot leave a full CPU batch for the next primitive");
    }else if(!strcmp(argv[1],"--readback")){
        unsigned char out[4]={0x5a,0x5a,0x5a,0x5a};
        for(unsigned submit=0;submit<2;++submit){
            fail_submit=submit;fail_wait=!submit;map_calls=0;
            assert(!DownloadVramRegionAsRGBA8888(0,0,1,1,out));
            assert(!map_calls && !pending_cmd);
            for(unsigned i=0;i<sizeof out;++i)assert(out[i]==0x5a);
        }
        fail_submit=fail_wait=false;
        puts("Failed submission/wait never maps or exposes unfinished VRAM data");
    }else if(!strcmp(argv[1],"--reset")){
        PS1_RECT region={0,0,8,8};
        Draw_ClearImage(&region,255,0,0);assert(pending_cmd);
        ResetPlatform();assert(!pending_cmd);
        assert(InitPlatform());
        Draw_ClearImage(&region,255,0,0);
        unsigned char pixel[4]={0};
        assert(DownloadVramRegionAsRGBA8888(0,0,1,1,pixel));
        assert(pixel[0]>200 && pixel[1]==0 && pixel[2]==0 && !pending_cmd);
        puts("Device reset forgets old command buffers and permits reinitialization");
    }else assert(0);
    QuitPlatform();return 0;
}

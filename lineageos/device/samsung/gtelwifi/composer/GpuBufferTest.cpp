// SPDX-License-Identifier: Apache-2.0
// Offscreen cache-ownership probe. No audio, no KMS changes, no VSP engine.
// Reads only its own imported CMA allocation through /dev/mem for comparison.
#define _LARGEFILE64_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#ifndef GPU_BUFFER_TEST_NDK
#include <vndk/hardware_buffer.h>
#endif
#ifndef GPU_BUFFER_TEST_NDK
#include <cutils/native_handle.h>
#else
struct native_handle_t { int version, numFds, numInts; int data[0]; };
extern "C" const native_handle_t* AHardwareBuffer_getNativeHandle(const AHardwareBuffer*);
#endif
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct VspImport { int32_t fd; uint32_t dma_addr, size, buffer_id; };
struct VspSync { uint32_t buffer_id, cpu; };
#define IMPORT _IOWR('m', 33, VspImport)
#define SYNC _IOW('m', 35, VspSync)
#define RELEASE_IMPORT _IOW('m', 34, uint32_t)
static void fail(const char* what) {
  fprintf(stderr, "%s: errno=%d EGL=%x GL=%x\n",what,errno,eglGetError(),glGetError());
  exit(1);
}
static void draw(float red, float green, float blue) {
  glViewport(0,0,800,1280);
  glClearColor(red,green,blue,1);
  glClear(GL_COLOR_BUFFER_BIT);
  glFinish();
  if(glGetError()!=GL_NO_ERROR) fail("GL draw");
}
static void draw_pattern() {
  // Integer scissor bounds avoid Mali-400 fragment-coordinate rounding near
  // y=1024; the reference pattern must not depend on shader float precision.
  glViewport(0,0,800,1280); glDisable(GL_DITHER); glEnable(GL_SCISSOR_TEST);
  for(unsigned y=0;y<1280;y+=8) for(unsigned x=0;x<800;x+=8) {
    unsigned cx=x/8,cy=y/8;
    glScissor(x,y,8,8);
    glClearColor(cx&1,cy&1,((cx/2)+(cy/2))&1,1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glDisable(GL_SCISSOR_TEST); glFinish();
  if(glGetError()!=GL_NO_ERROR) fail("GPU pattern");
}
static uint32_t pattern_pixel(unsigned x, unsigned y) {
  unsigned cx=x/8,cy=y/8;
  return 0xff000000u | ((cx&1)?0xffu:0) | ((cy&1)?0xff00u:0) |
         ((((cx/2)+(cy/2))&1)?0xff0000u:0);
}
int main(int argc, char** argv) {
  setbuf(stdout,nullptr);
  const bool client_pattern=argc==3 && !std::strcmp(argv[1],"client-pattern");
  const bool stress_offset=argc==3 && !std::strcmp(argv[1],"stress-offset");
  const unsigned expected_prefix=(client_pattern||stress_offset)?std::strtoul(argv[2],nullptr,0):0;
  if(expected_prefix>65536 || expected_prefix%4) fail("expected client prefix");
  AHardwareBuffer_Desc desc{};
  desc.width=800; desc.height=1280; desc.layers=1;
  if(stress_offset) desc.height+=(expected_prefix+3199)/3200;
  desc.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  desc.usage=AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
             AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
             AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
             AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
  if(client_pattern) desc.usage|=0x1000u; // Internal GRALLOC_USAGE_HW_FB.
  AHardwareBuffer* ahb=nullptr;
  if(AHardwareBuffer_allocate(&desc,&ahb)) fail("AHB allocate");
  AHardwareBuffer_describe(ahb,&desc);
  const native_handle_t* handle=AHardwareBuffer_getNativeHandle(ahb);
  if(!handle || handle->numFds<1) fail("AHB fd");
  int vsp=open("/dev/sprd_vsp",O_RDWR|O_CLOEXEC);
  if(vsp<0) fail("VSP allocator fd");
  VspImport imported{handle->data[0],0,0,0};
  if(ioctl(vsp,IMPORT,&imported)) fail("import own CMA buffer");
  printf("AHB=%ux%u stride=%u size=%u DMA=%08x id=%u\n",
         desc.width,desc.height,desc.stride,imported.size,imported.dma_addr,imported.buffer_id);
  if(imported.size<uint64_t(desc.stride)*1280*4+expected_prefix || imported.size>16*1024*1024)
    fail("allocation bounds");

  EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major,minor,count;
  if(!eglInitialize(display,&major,&minor)) fail("eglInitialize");
  const EGLint config_attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,
      EGL_BLUE_SIZE,8,EGL_NONE};
  EGLConfig config;
  if(!eglChooseConfig(display,config_attrs,&config,1,&count) || count!=1) fail("EGL config");
  const EGLint surface_attrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
  EGLSurface surface=eglCreatePbufferSurface(display,config,surface_attrs);
  const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);
  if(surface==EGL_NO_SURFACE || context==EGL_NO_CONTEXT ||
      !eglMakeCurrent(display,surface,surface,context)) fail("EGL context");
  printf("GL vendor=%s renderer=%s\n",glGetString(GL_VENDOR),glGetString(GL_RENDERER));
  auto get_buffer=reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
      eglGetProcAddress("eglGetNativeClientBufferANDROID"));
  auto create_image=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  auto destroy_image=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  auto image_storage=reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
      eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
  if(!get_buffer || !create_image || !destroy_image || !image_storage) fail("EGL image extensions");
  const EGLint image_attrs[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};
  const EGLint dma_attrs[]={EGL_WIDTH,800,EGL_HEIGHT,1280,
      EGL_LINUX_DRM_FOURCC_EXT,0x34324241,EGL_DMA_BUF_PLANE0_FD_EXT,handle->data[0],
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,EGLint(expected_prefix),
      EGL_DMA_BUF_PLANE0_PITCH_EXT,EGLint(desc.stride*4),EGL_NONE};
  EGLImageKHR image=stress_offset
      ? create_image(display,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,nullptr,dma_attrs)
      : create_image(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,get_buffer(ahb),image_attrs);
  if(image==EGL_NO_IMAGE_KHR) fail("EGL native image");
  GLuint renderbuffer,framebuffer;
  glGenRenderbuffers(1,&renderbuffer); glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
  image_storage(GL_RENDERBUFFER,image);
  glGenFramebuffers(1,&framebuffer); glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,renderbuffer);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) fail("GL framebuffer");
  void* mapping=mmap(nullptr,imported.size,PROT_READ|PROT_WRITE,MAP_SHARED,handle->data[0],0);
  if(mapping==MAP_FAILED) fail("CPU mmap");
  auto* cpu=static_cast<volatile uint32_t*>(mapping);
  int mem=open("/dev/mem",O_RDONLY|O_SYNC|O_CLOEXEC);
  if(mem<0) fail("own CMA physical read");
  uint64_t base=uint64_t(imported.dma_addr)&~uint64_t(4095);
  const size_t physical_size=imported.size+(imported.dma_addr&4095);
  void* physical=mmap64(nullptr,physical_size,PROT_READ,MAP_SHARED,mem,base);
  if(physical==MAP_FAILED) fail("own CMA physical mmap");
  auto* ram=reinterpret_cast<volatile uint32_t*>(
      static_cast<uint8_t*>(physical)+(imported.dma_addr&4095)+expected_prefix);

  const bool pattern_only=client_pattern || (argc==2 && !std::strcmp(argv[1],"pattern"));
  const bool stress_only=stress_offset || (argc==2 && !std::strcmp(argv[1],"stress"));
  cpu_set_t affinity; CPU_ZERO(&affinity); CPU_SET(3,&affinity);
  printf("CPU3 affinity=%d\n",sched_setaffinity(0,sizeof(affinity),&affinity));
  unsigned reproduced=0,preserved=0,inconclusive=0;
  constexpr unsigned kRounds=16;
  for(unsigned round=0;!pattern_only && !stress_only && round<kRounds;++round) {
    draw(1,0,0);
    if(ram[0]!=0xff0000ffu) fail("GPU red physical output");
    dma_buf_sync begin{DMA_BUF_SYNC_START|DMA_BUF_SYNC_RW};
    if(ioctl(handle->data[0],DMA_BUF_IOCTL_SYNC,&begin)) fail("DMA_BUF_SYNC START");
    // One cache line stays hot; a full-buffer memset can evict it itself.
    std::memset(mapping,0,64);
    __sync_synchronize();
    dma_buf_sync end{DMA_BUF_SYNC_END|DMA_BUF_SYNC_RW};
    if(ioctl(handle->data[0],DMA_BUF_IOCTL_SYNC,&end)) fail("DMA_BUF_SYNC END");
    draw(0,1,0);
    uint32_t before=ram[0],cached=cpu[0];
    VspSync sync{imported.buffer_id,0};
    if(ioctl(vsp,SYNC,&sync)) fail("same device cache clean as scanout");
    uint32_t after=ram[0];
    if(before==0xff00ff00u && after!=before) ++reproduced;
    else if(before==0xff00ff00u && cached==before && after==before) ++preserved;
    else ++inconclusive;
    printf("round=%u GPUgreen_RAM=%08x CPU=%08x after_clean_RAM=%08x\n",
           round,before,cached,after);
  }
  if(!pattern_only && !stress_only) {
  printf("SUMMARY rounds=%u clobbered=%u preserved=%u inconclusive=%u\n",
         kRounds,reproduced,preserved,inconclusive);
  if(reproduced) printf("REPRODUCED: stale dirty CPU cache overwrote completed GPU output\n");
  else if(preserved==kRounds) printf("PASS: CPU/device ownership preserved GPU output\n");
  else printf("INCONCLUSIVE: inspect physical and CPU values\n");
  }

  unsigned wrong=0,wrong_flipped=0;
  if(stress_only) {
    printf("GPU_STRESS logical_prefix=%u own_allocation_bytes=%u\n",expected_prefix,imported.size);
    timespec origin{}; clock_gettime(CLOCK_MONOTONIC,&origin);
    for(unsigned frame=0;frame<300;++frame) {
      draw(frame&1,!(frame&1),0);
      if(frame<2 && ram[0]!=(frame&1?0xff0000ffu:0xff00ff00u)) fail("GPU stress physical pixel");
      usleep(33333);
      if(frame%30==29) {
        timespec now{}; clock_gettime(CLOCK_MONOTONIC,&now);
        long long elapsed=(now.tv_sec-origin.tv_sec)*1000LL+(now.tv_nsec-origin.tv_nsec)/1000000;
        printf("GPU_STRESS frames=%u elapsed_ms=%lld\n",frame+1,elapsed);
      }
    }
    printf("PASS: 300 full-buffer offscreen GPU frames; no KMS changes\n");
  } else {
  draw_pattern();
  for(unsigned y=0;y<desc.height;++y) for(unsigned x=0;x<desc.width;++x) {
    uint32_t actual=ram[y*desc.stride+x],expected=pattern_pixel(x,y);
    if(actual!=expected) {
      if(wrong<8) printf("pixel x=%u y=%u RAM=%08x expected=%08x\n",x,y,actual,expected);
      ++wrong;
    }
    if(actual!=pattern_pixel(x,desc.height-1-y)) ++wrong_flipped;
  }
  printf("GPU_PATTERN pixels=%u mismatch_linear=%u mismatch_y_flipped=%u stride=%u\n",
         desc.width*desc.height,wrong,wrong_flipped,desc.stride);
  printf("%s: GPU 8x8 pattern has %s linear native memory layout\n",
         !wrong || !wrong_flipped ? "PASS" : "FAIL",wrong_flipped==0 ? "Y-flipped" : "normal");
  if(client_pattern) for(unsigned pass=0;pass<2;++pass) {
    void* locked=nullptr;
    if(AHardwareBuffer_lock(ahb,AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,-1,nullptr,&locked))
      fail("client CPU lock");
    unsigned cpu_wrong=0;
    const auto* locked_pixels=static_cast<const uint32_t*>(locked);
    for(unsigned y=0;y<desc.height;++y) for(unsigned x=0;x<desc.width;++x)
      if(locked_pixels[y*desc.stride+x]!=pattern_pixel(x,y)) ++cpu_wrong;
    if(AHardwareBuffer_unlock(ahb,nullptr)) fail("client CPU unlock");
    printf("CLIENT_PREFIX expected=%u pass=%u cpu_lock_pattern_mismatches=%u (includes final pixel)\n",
           expected_prefix,pass,cpu_wrong);
    if(cpu_wrong) wrong=wrong_flipped=cpu_wrong;
  }
  }

  munmap(physical,physical_size); close(mem); munmap(mapping,imported.size);
  glDeleteFramebuffers(1,&framebuffer); glDeleteRenderbuffers(1,&renderbuffer);
  destroy_image(display,image); eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
  if(ioctl(vsp,RELEASE_IMPORT,&imported.buffer_id)) fail("release import");
  close(vsp); AHardwareBuffer_release(ahb);
  return wrong && wrong_flipped ? 1 : 0;
}

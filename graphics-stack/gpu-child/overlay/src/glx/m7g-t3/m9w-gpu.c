/* Private opt-in GL producer for the existing OPAQUE_FD buffer protocol.
 * No pixel map/readback/upload on CPU. Imported objects belong to one context.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <GL/glcorearb.h>
#include <GL/glext.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <sys/syscall.h>
#include "m9w-gpu.h"
#define N 3
#define DECL(type,name) type name
struct m9w_gpu {
 m7g_t3_xport *x;void *owner;GLuint memory[N],pbo[N],ready[N],returned[N];
 unsigned used[N],returned_consumed[N],frames;unsigned w,h;unsigned trace_slot;int wait_ready;
 DECL(PFNGLGETINTEGERVPROC,GetIntegerv);DECL(PFNGLGETERRORPROC,GetError);
 DECL(PFNGLGETSTRINGPROC,GetString);DECL(PFNGLGETSTRINGIPROC,GetStringi);
 DECL(PFNGLGETUNSIGNEDBYTEVEXTPROC,GetUnsignedBytevEXT);DECL(PFNGLGETUNSIGNEDBYTEI_VEXTPROC,GetUnsignedBytei_vEXT);
 DECL(PFNGLCREATEMEMORYOBJECTSEXTPROC,CreateMemoryObjectsEXT);DECL(PFNGLMEMORYOBJECTPARAMETERIVEXTPROC,MemoryObjectParameterivEXT);
 DECL(PFNGLIMPORTMEMORYFDEXTPROC,ImportMemoryFdEXT);DECL(PFNGLBUFFERSTORAGEMEMEXTPROC,BufferStorageMemEXT);
 DECL(PFNGLGENBUFFERSPROC,GenBuffers);DECL(PFNGLBINDBUFFERPROC,BindBuffer);
 DECL(PFNGLGENSEMAPHORESEXTPROC,GenSemaphoresEXT);DECL(PFNGLIMPORTSEMAPHOREFDEXTPROC,ImportSemaphoreFdEXT);
 DECL(PFNGLWAITSEMAPHOREEXTPROC,WaitSemaphoreEXT);DECL(PFNGLSIGNALSEMAPHOREEXTPROC,SignalSemaphoreEXT);
 DECL(PFNGLBINDFRAMEBUFFERPROC,BindFramebuffer);DECL(PFNGLREADBUFFERPROC,ReadBuffer);DECL(PFNGLPIXELSTOREIPROC,PixelStorei);
 DECL(PFNGLREADPIXELSPROC,ReadPixels);DECL(PFNGLMEMORYBARRIERPROC,MemoryBarrier);DECL(PFNGLFLUSHPROC,Flush);
 DECL(PFNGLFENCESYNCPROC,FenceSync);DECL(PFNGLCLIENTWAITSYNCPROC,ClientWaitSync);DECL(PFNGLDELETESYNCPROC,DeleteSync);
 DECL(PFNGLDELETEBUFFERSPROC,DeleteBuffers);DECL(PFNGLDELETEMEMORYOBJECTSEXTPROC,DeleteMemoryObjectsEXT);
 DECL(PFNGLDELETESEMAPHORESEXTPROC,DeleteSemaphoresEXT);
 DECL(PFNGLOBJECTLABELPROC,ObjectLabel);
};
void m9w_log(const char *fmt,...){
 char buf[1024];va_list ap;va_start(ap,fmt);int n=vsnprintf(buf,sizeof(buf),fmt,ap);va_end(ap);
 if(n<0)return;
 if(n>=(int)sizeof(buf))n=sizeof(buf)-1;
 const char *path=getenv("M9W_GPU_LOG");if(!path)return;
 int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
 if(fd>=0){struct timespec ts={0};clock_gettime(CLOCK_MONOTONIC,&ts);dprintf(fd,"pid=%ld t_ns=%llu %.*s\n",(long)getpid(),(unsigned long long)ts.tv_sec*1000000000ull+ts.tv_nsec,n,buf);close(fd);}
}
static void api(struct m9w_gpu *g,const char *stage,const char *edge) {
 /* Narrow-window trace remains on throughout menu interaction. Large-window
  * first frames + periodic samples bound the instrumentation overhead. */
 if ((g->w<500 && g->h>400) || g->frames<16 || g->frames%120==0)
  m9w_log("M10K_API tid=%ld context=%p frame=%u slot=%u extent=%ux%u stage=%s edge=%s",
           (long)syscall(SYS_gettid),g->owner,g->frames,g->trace_slot,g->w,g->h,stage,edge);
}
static int glerr(struct m9w_gpu *g,const char *s){GLenum e=g->GetError();if(e){m9w_log("GPU_FAIL stage=%s gl=0x%x",s,e);return 93;}return 0;}
static int complete(struct m9w_gpu *g,const char *stage){
 api(g,"FenceSync","BEGIN");GLsync f=g->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);api(g,"FenceSync","END");if(!f)return 93;
 api(g,"CompleteFlush","BEGIN");g->Flush();api(g,"CompleteFlush","END");api(g,"ClientWait3s","BEGIN");GLenum r=g->ClientWaitSync(f,GL_SYNC_FLUSH_COMMANDS_BIT,3000000000ull);api(g,"ClientWait3s","END");
 if(r!=GL_ALREADY_SIGNALED&&r!=GL_CONDITION_SATISFIED){
  /* M9W producer fence slow (>3s); waiting up to +27s: under heavy system load
   * the consumer can be late by seconds; keep waiting before failing closed. */
  m9w_log("GPU_SLOW stage=%s fence=0x%x; waiting up to +27s; frame=%u slot=%u extent=%ux%u context=%p",stage,r,g->frames,g->trace_slot,g->w,g->h,g->owner);
  api(g,"ClientWait27s","BEGIN");r=g->ClientWaitSync(f,GL_SYNC_FLUSH_COMMANDS_BIT,27000000000ull);api(g,"ClientWait27s","END");
 }
 g->DeleteSync(f);
 if(r!=GL_ALREADY_SIGNALED&&r!=GL_CONDITION_SATISFIED){m9w_log("GPU_FAIL stage=%s fence=0x%x",stage,r);return 94;}
 return glerr(g,stage);
}
static int wait_returned(struct m9w_gpu *g,unsigned slot,const char *stage){
 g->trace_slot=slot;api(g,"wait_returned","BEGIN");
 /* Mesa24.2.8 zink_fence_server_sync transfers mfence->sem into a batch,
  * clears it and treats subsequent waits of that GL object as no-ops.
  * Reimport a fresh semaphore object for each subsequent wait. The original
  * Vulkan binary payload is shared; only FD/object metadata is recreated. */
 if(g->returned_consumed[slot]){
  g->DeleteSemaphoresEXT(1,&g->returned[slot]);g->GenSemaphoresEXT(1,&g->returned[slot]);
  int fd=m9w_xport_returned_fd(g->x,slot);if(fd<0)return 93;
  api(g,"ImportReturned","BEGIN");g->ImportSemaphoreFdEXT(g->returned[slot],GL_HANDLE_TYPE_OPAQUE_FD_EXT,fd);api(g,"ImportReturned","END");
  if(glerr(g,"reimport-returned")){close(fd);return 93;}
 }
 api(g,"WaitReturned","BEGIN");g->WaitSemaphoreEXT(g->returned[slot],1,&g->pbo[slot],0,NULL,NULL);api(g,"WaitReturned","END");
 int rc=complete(g,stage);if(!rc)g->returned_consumed[slot]=1;api(g,"wait_returned","END");return rc;
}
int m9w_gpu_init(m9w_gpu **out,m7g_t3_xport *x,m9w_getproc proc,void *owner){
 struct m9w_gpu *g=calloc(1,sizeof(*g));if(!g)return 93;g->x=x;g->owner=owner;g->w=m7g_t3_xport_w(x);g->h=m7g_t3_xport_h(x);g->wait_ready=getenv("M10K_WAIT_READY") && !strcmp(getenv("M10K_WAIT_READY"),"1");
#define L(name) do{g->name=(__typeof__(g->name))proc("gl"#name);if(!g->name){m9w_log("GPU_FAIL missing=gl%s",#name);return 93;}}while(0)
 L(GetIntegerv);L(GetError);L(GetString);L(GetStringi);L(GetUnsignedBytevEXT);L(GetUnsignedBytei_vEXT);
 L(CreateMemoryObjectsEXT);L(MemoryObjectParameterivEXT);L(ImportMemoryFdEXT);L(BufferStorageMemEXT);L(GenBuffers);L(BindBuffer);
 L(GenSemaphoresEXT);L(ImportSemaphoreFdEXT);L(WaitSemaphoreEXT);L(SignalSemaphoreEXT);L(BindFramebuffer);L(ReadBuffer);L(PixelStorei);L(ReadPixels);
 L(MemoryBarrier);L(Flush);L(FenceSync);L(ClientWaitSync);L(DeleteSync);L(DeleteBuffers);L(DeleteMemoryObjectsEXT);L(DeleteSemaphoresEXT);
 L(ObjectLabel);
#undef L
 for(unsigned n=0;g->GetError()!=GL_NO_ERROR;n++)if(n>=16)return 93;
 const char *renderer=(const char*)g->GetString(GL_RENDERER);if(!renderer||!strstr(renderer,"zink"))return 93;
 GLint ne=0;g->GetIntegerv(GL_NUM_EXTENSIONS,&ne);const char *need[]={"GL_EXT_memory_object_fd","GL_EXT_semaphore_fd"};
 for(unsigned j=0;j<2;j++){int have=0;for(GLint i=0;i<ne;i++)if(!strcmp((const char*)g->GetStringi(GL_EXTENSIONS,i),need[j]))have=1;if(!have)return 93;}
 GLint old_pbo=0;g->GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&old_pbo);
 g->CreateMemoryObjectsEXT(N,g->memory);g->GenBuffers(N,g->pbo);g->GenSemaphoresEXT(N,g->ready);g->GenSemaphoresEXT(N,g->returned);
 if(glerr(g,"create-GL-objects"))return 93;
 for(unsigned i=0;i<N;i++){
  struct m9w_slot_export e;if(m9w_xport_export(x,i,&e))return 93;
  GLubyte uuid[16];GLint count=0;g->GetIntegerv(GL_NUM_DEVICE_UUIDS_EXT,&count);int match=0;
  for(GLint k=0;k<count;k++){g->GetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT,k,uuid);if(!memcmp(uuid,e.device_uuid,16))match=1;}
  g->GetUnsignedBytevEXT(GL_DRIVER_UUID_EXT,uuid);
  if(!match||memcmp(uuid,e.driver_uuid,16)){close(e.memory_fd);close(e.ready_fd);close(e.returned_fd);return 93;}
  if(e.dedicated){GLint yes=GL_TRUE;g->MemoryObjectParameterivEXT(g->memory[i],GL_DEDICATED_MEMORY_OBJECT_EXT,&yes);}
  g->ImportMemoryFdEXT(g->memory[i],e.allocation_size,GL_HANDLE_TYPE_OPAQUE_FD_EXT,e.memory_fd);
  if(glerr(g,"import-memory")){close(e.memory_fd);close(e.ready_fd);close(e.returned_fd);return 93;}
  g->BindBuffer(GL_PIXEL_PACK_BUFFER,g->pbo[i]);g->BufferStorageMemEXT(GL_PIXEL_PACK_BUFFER,e.bytes,g->memory[i],0);
  if(glerr(g,"buffer-storage")){close(e.ready_fd);close(e.returned_fd);return 93;}
  g->ObjectLabel(GL_BUFFER,g->pbo[i],-1,"M9W_GPU_SLOT");
  if(glerr(g,"buffer-label")){close(e.ready_fd);close(e.returned_fd);return 93;}
  g->ImportSemaphoreFdEXT(g->ready[i],GL_HANDLE_TYPE_OPAQUE_FD_EXT,e.ready_fd);
  if(glerr(g,"import-ready")){close(e.ready_fd);close(e.returned_fd);return 93;}
  g->ImportSemaphoreFdEXT(g->returned[i],GL_HANDLE_TYPE_OPAQUE_FD_EXT,e.returned_fd);
  if(glerr(g,"import-returned")){close(e.returned_fd);return 93;}
 }
 g->BindBuffer(GL_PIXEL_PACK_BUFFER,(GLuint)old_pbo);if(glerr(g,"init"))return 93;
 m9w_log("M10K_TRACE variant=v12-api-trace-v1 release_gate=0 cpu_pixel_copy=0");m9w_log("M10K_READY_COMPLETION enabled=%d",g->wait_ready);m9w_log("M10K_COPY_SPLIT enabled=%d",g->wait_ready);m9w_log("M10K_REASSERT_VARIANT enabled=1 release_bound_ms=2000");
 m9w_log("GPU_INIT w=%u h=%u slots=3 renderer=%s context=%p",g->w,g->h,renderer,owner);*out=g;return 0;
}
int m9w_gpu_write(m9w_gpu *g,unsigned slot,void *owner){return m9w_gpu_write_fb(g,slot,owner,0,GL_BACK);}
int m9w_gpu_write_fb(m9w_gpu *g,unsigned slot,void *owner,unsigned fb,unsigned read){
 if(!g||slot>=N||g->owner!=owner){m9w_log("GPU_FAIL stage=context-mismatch");return 93;}
 g->trace_slot=slot;api(g,"write","BEGIN");
 for(unsigned pending=0;g->GetError()!=GL_NO_ERROR;pending++)if(pending>=16)return 93;
 int link_rc=m10k_xport_reassert(g->x);if(link_rc)return link_rc;
 int release_rc=m10k_xport_release_gate(g->x,slot);if(release_rc)return release_rc;
 if(g->used[slot]){int rc=wait_returned(g,slot,"returned");if(rc)return rc;g->used[slot]=0;}
 GLint old_fb=0,old_read=0,old_pbo=0;g->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&old_fb);g->BindFramebuffer(GL_READ_FRAMEBUFFER,fb);g->GetIntegerv(GL_READ_BUFFER,&old_read);g->GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&old_pbo);
 GLenum fields[]={GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS,GL_PACK_IMAGE_HEIGHT,GL_PACK_SKIP_IMAGES,GL_PACK_LSB_FIRST,GL_PACK_SWAP_BYTES};GLint values[8];
 for(unsigned i=0;i<8;i++){g->GetIntegerv(fields[i],&values[i]);g->PixelStorei(fields[i],i==0?4:0);}
 g->ReadBuffer(read);g->BindBuffer(GL_PIXEL_PACK_BUFFER,g->pbo[slot]);api(g,"ReadPixelsPBO","BEGIN");g->ReadPixels(0,0,g->w,g->h,GL_RGBA,GL_UNSIGNED_BYTE,0);api(g,"ReadPixelsPBO","END");
 if(glerr(g,"PBO-readpixels"))return 93;
 api(g,"MemoryBarrier","BEGIN");g->MemoryBarrier(GL_PIXEL_BUFFER_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT);api(g,"MemoryBarrier","END");
 if(g->wait_ready) {
  api(g,"PBOBeforeReadyComplete","BEGIN");
  int rc=complete(g,"pbo-before-ready");
  api(g,"PBOBeforeReadyComplete","END");
  if(rc)return rc;
 }
 api(g,"SignalReady","BEGIN");g->SignalSemaphoreEXT(g->ready[slot],1,&g->pbo[slot],0,NULL,NULL);api(g,"SignalReady","END");api(g,"WriteFlush","BEGIN");g->Flush();api(g,"WriteFlush","END");
 if(glerr(g,"signal-ready"))return 93;
 for(unsigned i=0;i<8;i++)g->PixelStorei(fields[i],values[i]);
 g->BindBuffer(GL_PIXEL_PACK_BUFFER,(GLuint)old_pbo);g->ReadBuffer((GLenum)old_read);g->BindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)old_fb);
 if(glerr(g,"state-restore"))return 93;
 /* Signal-ready was submitted above. Complete that same GL command stream
  * before the adapter can send the Wayland commit or switch drawables.
  * No returned wait is removed, signalled on CPU, or weakened. */
 if(g->wait_ready) {
  m9w_log("M10K_READY_WAIT edge=BEGIN frame=%u slot=%u extent=%ux%u context=%p",g->frames,slot,g->w,g->h,g->owner);
  int rc=complete(g,"producer-ready");
  m9w_log("M10K_READY_WAIT edge=END rc=%d frame=%u slot=%u extent=%ux%u context=%p",rc,g->frames,slot,g->w,g->h,g->owner);
  if(rc)return rc;
 }
 api(g,"write","END");g->used[slot]=1;g->frames++;if(g->frames<=8||g->frames%120==0)m9w_log("GPU_WRITE frame=%u slot=%u w=%u h=%u cpu_payload_bytes=0",g->frames,slot,g->w,g->h);
 return 0;
}
int m9w_gpu_drain(m9w_gpu *g,void *owner){
 if(!g)return 0;
 if(owner!=g->owner){m9w_log("GPU_FAIL stage=destroy-context-mismatch");return 93;}
 for(unsigned i=0;i<N;i++)if(g->used[i]){
  int rc=m10k_xport_release_gate(g->x,i);if(rc)return rc;
  rc=wait_returned(g,i,"teardown-returned");if(rc)return rc;
  if(m9w_xport_idle(g->x,i))return 93;
  g->used[i]=0;
 }
 if(complete(g,"teardown-GL"))return 93;
 return 0;
}
int m9w_gpu_destroy(m9w_gpu *g,void *owner){
 if(!g)return 0;
 int rc=m9w_gpu_drain(g,owner);if(rc)return rc;
 g->DeleteBuffers(N,g->pbo);g->DeleteMemoryObjectsEXT(N,g->memory);g->DeleteSemaphoresEXT(N,g->ready);g->DeleteSemaphoresEXT(N,g->returned);
 if(glerr(g,"cleanup"))return 93;
 m9w_log("GPU_CLEANUP OK frames=%u",g->frames);free(g);return 0;
}
int m9w_gpu_owned(m9w_gpu *g,void *owner){return g&&g->owner==owner;}

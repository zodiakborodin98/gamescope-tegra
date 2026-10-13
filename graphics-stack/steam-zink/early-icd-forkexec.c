/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Private native Steam/Zink bootstrap, derived from the proven FEX boundary.
 * No GLX/provider/extension spoof. No WSI surface/swapchain/present calls.
 * DISPLAY is hidden only during real NVIDIA load/create/enumerate, then restored.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <stdarg.h>
#include <ctype.h>

static int fd=-1,armed,mutations;
static pid_t owner;
static int threads(void) {
 FILE *f=fopen("/proc/self/status","r");if(!f)return -1;
 char b[256];int n=-1;
 while(fgets(b,sizeof b,f))if(sscanf(b,"Threads: %d",&n)==1)break;
 fclose(f);return n;
}
static void logline(const char *format,...) {
 char b[1400];int n=snprintf(b,sizeof b,"[steam-zink pid=%ld tid=%ld threads=%d] ",(long)getpid(),(long)syscall(SYS_gettid),threads());
 va_list a;va_start(a,format);int m=vsnprintf(b+n,sizeof b-(size_t)n,format,a);va_end(a);
 if(m>0)n+=m;
 if(n>=(int)sizeof b)n=(int)sizeof b-1;
 b[n++]='\n';
 if(fd>=0){ssize_t r=write(fd,b,(size_t)n);(void)r;}
}
static void deadline(int sig) {
 (void)sig;static const char s[]="STEAM_ZINK_EARLY_TIMEOUT exit=97; process terminated, no restore claim\n";
 if(fd>=0){ssize_t r=write(fd,s,sizeof s-1);(void)r;}_exit(97);
}
static void fork_exec_deadline(int sig) {
 (void)sig;static const char msg[]="STEAM_ZINK_FORK_EXEC_TIMEOUT exit=95\n";
 if(fd>=0){ssize_t r=write(fd,msg,sizeof msg-1);(void)r;}_exit(95);
}
static void cef_fork_child(void) {
 /* Only async-signal-safe operations. No GPU call, destructor or env mutation.
  * Chrome's no-zygote launcher forks briefly, then executes a fresh image.
  * The owned exec constructor cancels this deadline BEFORE any ICD work.
  */
 char msg[96]="STEAM_ZINK_FORK_EXEC_PENDING child=";size_t n=sizeof("STEAM_ZINK_FORK_EXEC_PENDING child=")-1;
 unsigned long id=(unsigned long)getpid();char digits[24];size_t k=0;
 do{digits[k++]=(char)('0'+id%10);id/=10;}while(id);
 while(k){msg[n++]=digits[--k];}msg[n++]='\n';
 if(fd>=0){ssize_t r=write(fd,msg,n);(void)r;}
 struct sigaction a={0};a.sa_handler=fork_exec_deadline;sigemptyset(&a.sa_mask);
 if(sigaction(SIGALRM,&a,NULL))_exit(95);
 alarm(5);
}
static int arg_exact(const char *raw,size_t n,const char *wanted) {
 size_t i=0;while(i<n){size_t k=strlen(raw+i);if(!strcmp(raw+i,wanted))return 1;i+=k+1;}return 0;
}
__attribute__((constructor)) static void begin(void) {
 const char *token=getenv("STEAM_ZINK_TOKEN");if(!token)return;
 const char *top=getenv("STEAM_ZINK_EXEC_TOPOLOGY");
 if(top && !strcmp(top,"direct-v1"))alarm(0); /* Fresh exec image; no inherited context. */
 char exe[4096],raw[16384];ssize_t x=readlink("/proc/self/exe",exe,sizeof exe-1);
 if(x<=0 || x>=(ssize_t)sizeof exe-1)_exit(95);
 exe[x]=0;
 const char *alt=getenv("STEAM_ZINK_CEF_ALT_EXE");
 const char *steam=getenv("STEAM_ZINK_STEAM_EXE"),*cef=getenv("STEAM_ZINK_CEF_EXE"),*probe=getenv("STEAM_ZINK_PROBE_EXE");
 int iscef=(cef && !strcmp(exe,cef)) || (alt && !strcmp(exe,alt));
 if(!iscef && !(steam && !strcmp(exe,steam)) && !(probe && !strcmp(exe,probe)))return;
 const char *path=getenv("STEAM_ZINK_BOOT_LOG");if(!path || path[0]!='/')_exit(95);
 fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)_exit(95);
 if(strlen(token)!=32)_exit(95);
 for(size_t i=0;i<32;i++)if(!isxdigit((unsigned char)token[i]))_exit(95);
 FILE *f=fopen("/proc/self/cmdline","rb");if(!f)_exit(95);size_t n=fread(raw,1,sizeof raw,f);int err=ferror(f);fclose(f);
 if(err || !n || n==sizeof raw || raw[n-1])_exit(95);
 if(iscef && (arg_exact(raw,n,"--type=zygote") || arg_exact(raw,n,"--single-process") ||
      !getenv("STEAM_ZINK_EXEC_TOPOLOGY") || strcmp(getenv("STEAM_ZINK_EXEC_TOPOLOGY"),"direct-v1") ||
      (!arg_exact(raw,n,"--no-zygote") && !arg_exact(raw,n,"--type=renderer") &&
       !arg_exact(raw,n,"--type=gpu-process") && !arg_exact(raw,n,"--type=utility")))) {
  logline("CEF_TOPOLOGY_REFUSED before ICD exit=95");_exit(95);
 }
 if(threads()!=1){logline("EARLY_APPLICATION_THREADS_REFUSED exit=96");_exit(96);}
 owner=getpid();armed=1;
 logline("BEGIN exe=%s token=%s variant=steam-zink-forkexec-v3",exe,token);
 const char *current=getenv("DISPLAY");char *saved=current?strdup(current):NULL;
 if(current && !saved)_exit(96);
 void *libc=dlopen("libc.so.6",RTLD_NOW|RTLD_LOCAL);
 void *(*realopen)(const char*,int)=libc?(void*(*)(const char*,int))dlsym(libc,"dlopen"):NULL;
 void *(*realsym)(void*,const char*)=libc?(void*(*)(void*,const char*))dlsym(libc,"dlsym"):NULL;
 if(!realopen || !realsym){logline("REAL_LIBC_REFUSED exit=96");_exit(96);}
 const char *json=getenv("VK_DRIVER_FILES");if(!json || json[0]!='/')json="/etc/vulkan/icd.d/nvidia_icd.json";
 char text[8192],icd[4096];f=fopen(json,"r");if(!f)_exit(96);n=fread(text,1,sizeof text-1,f);err=ferror(f);fclose(f);text[n]=0;
 char *s=strstr(text,"\"library_path\"");s=s?strchr(s,':'):NULL;s=s?strchr(s,'"'):NULL;
 char *end=s?strchr(++s,'"'):NULL;
 if(err || !s || !end || end-s<=0 || end-s>=(int)sizeof icd){logline("ICD_JSON_REFUSED exit=96");_exit(96);}
 memcpy(icd,s,(size_t)(end-s));icd[end-s]=0;
 struct sigaction old,sa={0};sa.sa_handler=deadline;sigemptyset(&sa.sa_mask);
 if(sigaction(SIGALRM,&sa,&old))_exit(96);
 alarm(25);
 if(unsetenv("DISPLAY"))_exit(96);
 mutations++;
 logline("HIDE DISPLAY; ICD=%s",icd);
 void *h=realopen(icd,RTLD_NOW|RTLD_LOCAL);const char *stage="dlopen";VkResult rc=VK_ERROR_INITIALIZATION_FAILED;uint32_t count=0;
 if(h){
  VkResult (*negotiate)(uint32_t*)=(VkResult(*)(uint32_t*))realsym(h,"vk_icdNegotiateLoaderICDInterfaceVersion");
  PFN_vkGetInstanceProcAddr gipa=(PFN_vkGetInstanceProcAddr)realsym(h,"vk_icdGetInstanceProcAddr");
  uint32_t v=5;stage="negotiate";
  if(negotiate && gipa && (rc=negotiate(&v))==VK_SUCCESS){
   PFN_vkCreateInstance create=(PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
   VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="steam-zink-early",.apiVersion=VK_API_VERSION_1_0};
   VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};VkInstance instance=VK_NULL_HANDLE;
   stage="create";
   if(create && (rc=create(&ci,NULL,&instance))==VK_SUCCESS && instance){
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)gipa(instance,"vkEnumeratePhysicalDevices");
    stage="enumerate";if(enumerate)rc=enumerate(instance,&count,NULL);else rc=VK_ERROR_INITIALIZATION_FAILED;
    /* Instance+ICD kept for this process lifetime, as in the qualified FEX path. */
   }
  }
 }
 int restored=saved?setenv("DISPLAY",saved,1):unsetenv("DISPLAY");mutations++;alarm(0);sigaction(SIGALRM,&old,NULL);
 logline("RESTORE DISPLAY=%s rc=%d stage=%s vk_rc=%d count=%u mutations=%d",saved?saved:"(unset)",restored,stage,(int)rc,count,mutations);free(saved);
 if(restored || strcmp(stage,"enumerate") || rc!=VK_SUCCESS || !count){logline("EARLY_FAIL exit=96");_exit(96);}
 if(iscef && pthread_atfork(NULL,NULL,cef_fork_child)){logline("CEF_FORK_GUARD_REFUSED exit=96");_exit(96);}
 logline("READY; no further DISPLAY mutations; cef_fork_guard=%d",iscef);
}
__attribute__((destructor)) static void done(void) {
 if(armed && owner!=getpid())_exit(95); /* No inherited GPU teardown without exec. */
 if(armed && owner==getpid())logline("PROCESS_END mutations=%d",mutations);
 if(fd>=0)close(fd);
}

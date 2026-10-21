// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7g - Gamescope EGLStream *consumer* for the Xwayland EGLStream path (server).
//
// Implements the server side of wl_eglstream_display (v1) and
// wl_eglstream_controller (v2): Xwayland hands us the stream fd (create_stream),
// we build the consumer side of the EGLStream (eglCreateStreamFromFileDescriptorKHR
// + eglStreamConsumerGLTextureExternalKHR) and attach it to the wl_surface's
// wl_buffer (attach_eglstream_consumer). Every committed frame is acquired in a
// DEDICATED consumer EGL/GL context (own thread), transferred FULL EXTENT into a
// compositor-owned OPAQUE_FD transport slot and then copied GPU-side into a fresh
// Gamescope-owned RGBA8 VkImage with the accepted buffer->image body, so it can
// flow through the ordinary composition path (commit->vulkanTex).
//
// Design decisions honoured (see reports/M7g-gamescope-consumer-integration.md):
//   §1 dedicated consumer thread owns EGLDisplay/context; attach posts connect
//      and BLOCKS (bounded) until it completes; import_commit posts acquire+blit
//      the same way. Standalone context: NO sharing with existing contexts is
//      required (Gamescope is Vulkan-only; the consumer GL objects all live here).
//   §2 per-generation sizing: one slot sized w*h*4 at attach, reused per frame,
//      freed at generation end; lazy alloc; kMaxDim/kMaxAlloc guards.
//   §3 connect completes BEFORE the attach handler returns (Xwayland sync `done`).
//   §4 connect failure -> fail-closed abort (no fake signals, no degradation).
//   §4/§5 transport seam = the exported slot entry added in the transport TU.
//   §5 external texture destroyed only on the owner thread after the last acquire
//      release + copy fence; the generation survives until the last copy fence and
//      wl_buffer.release. The 2 s acquire timeout fails closed. Per-wl_surface ->
//      per-generation keying; attached-but-never-committed teardown on the
//      wl_buffer / wl_surface destroy paths.
//
// PROBE-ONLY markers (_M7G_TEST_WINDOW, colour/size selection, FRAME-VERIFIED
// state file) do NOT appear here. No CPU framebuffer copy/readback in the display
// path (transform feedback writes the GPU buffer; the compositor copies on GPU).

// M9Y P1 (marker M9Y_P1_LIFETIME): job ownership and record lifetime port on
// the C04 retirement/refusal baseline. Jobs are owned by the consumer queue
// (shared_ptr); the record is finalized on the wlserver event loop only after
// the consumer-side teardown completed; owner-side copy/ack work is pinned.
// C04 retirement/refusal behaviour and the fail-closed policy are unchanged.
// See reports/M9Y-eglstream-c04-lifetime-p1.md.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "Backends/TegraL4TBackend.h"

#include "rendervulkan.hpp"
#include "wlserver.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "wlr_end.hpp"

#include "eglstream/wayland-eglstream-server-protocol.h"
#include "eglstream/wayland-eglstream-controller-server-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <sys/eventfd.h>
#include <vector>

#ifndef EGL_STREAM_BIT_KHR
#define EGL_STREAM_BIT_KHR 0x0400
#endif
#ifndef EGL_CONSUMER_ACQUIRE_TIMEOUT_USEC_KHR
#define EGL_CONSUMER_ACQUIRE_TIMEOUT_USEC_KHR 0x3216
#endif
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif

namespace gamescope
{
    // --------------------------------------------------------------------- logging
    static void tl4t_es_log( const char *pFmt, ... )
    {
        va_list args; va_start( args, pFmt );
        fprintf( stderr, "[tegra-l4t-egls] " );
        vfprintf( stderr, pFmt, args ); fprintf( stderr, "\n" );
        fflush( stderr );
        va_end( args );
    }

    // Fail-closed abort through the established hard-stop path: never fake a
    // signal or silently degrade (decision 4).
    static void tl4t_es_fail_closed( const char *pWhy )
    {
        tl4t_es_log( "FATAL: %s; fail-closed abort", pWhy ? pWhy : "?" );
        fflush( stderr );
        _exit( 3 );
    }

    // ------------------------------------------------- GLES3 local types/consts
    // The target sysroot has no GLES headers; resolve entry points through
    // eglGetProcAddress and declare only what this TU uses.
    typedef void              GLvoid;
    typedef char              GLchar;
    typedef unsigned int      GLenum;
    typedef unsigned char     GLboolean;
    typedef unsigned char     GLubyte;
    typedef unsigned int      GLbitfield;
    typedef int               GLint;
    typedef int               GLsizei;
    typedef unsigned int      GLuint;
    typedef float             GLfloat;
    typedef ptrdiff_t         GLintptr;
    typedef ptrdiff_t         GLsizeiptr;

    constexpr GLenum  GL_FALSE_                    = 0;
    constexpr GLenum  GL_TEXTURE_2D                = 0x0DE1;
    constexpr GLenum  GL_TEXTURE_EXTERNAL_OES      = 0x8D65;
    constexpr GLenum  GL_TEXTURE0                  = 0x84C0;
    constexpr GLenum  GL_TEXTURE_MIN_FILTER        = 0x2801;
    constexpr GLenum  GL_TEXTURE_MAG_FILTER        = 0x2800;
    constexpr GLenum  GL_TEXTURE_WRAP_S            = 0x2802;
    constexpr GLenum  GL_TEXTURE_WRAP_T            = 0x2803;
    constexpr GLenum  GL_LINEAR                    = 0x2601;
    constexpr GLenum  GL_CLAMP_TO_EDGE             = 0x812F;
    constexpr GLenum  GL_VERTEX_SHADER             = 0x8B31;
    constexpr GLenum  GL_FRAGMENT_SHADER           = 0x8B30;
    constexpr GLenum  GL_COMPILE_STATUS            = 0x8B81;
    constexpr GLenum  GL_LINK_STATUS               = 0x8B82;
    constexpr GLenum  GL_INFO_LOG_LENGTH           = 0x8B84;
    constexpr GLenum  GL_RASTERIZER_DISCARD        = 0x8C89;
    constexpr GLenum  GL_POINTS                    = 0x0000;
    constexpr GLenum  GL_INTERLEAVED_ATTRIBS       = 0x8C8C;
    constexpr GLenum  GL_TRANSFORM_FEEDBACK_BUFFER = 0x8C8E;
    constexpr GLenum  GL_MAJOR_VERSION             = 0x821B;
    constexpr GLenum  GL_MINOR_VERSION             = 0x821C;
    constexpr GLenum  GL_NUM_EXTENSIONS            = 0x821D;
    constexpr GLenum  GL_EXTENSIONS                = 0x1F03;
    constexpr GLenum  GL_ARRAY_BUFFER              = 0x8892;
    constexpr GLenum  GL_COPY_READ_BUFFER          = 0x8F36;
    constexpr GLenum  GL_COPY_WRITE_BUFFER         = 0x8F37;
    constexpr GLenum  GL_FRAMEBUFFER               = 0x8D40;
    constexpr GLenum  GL_RENDERBUFFER              = 0x8D41;
    constexpr GLenum  GL_RGBA8                     = 0x8058;
    constexpr GLenum  GL_COLOR_ATTACHMENT0         = 0x8CE0;
    constexpr GLenum  GL_FRAMEBUFFER_COMPLETE      = 0x8CD5;
    constexpr GLenum  GL_NO_ERROR                  = 0;
    constexpr GLenum  GL_DEDICATED_MEMORY_OBJECT_EXT = 0x9581;
    constexpr GLenum  GL_HANDLE_TYPE_OPAQUE_FD_EXT   = 0x9586;
    constexpr GLenum  GL_NUM_DEVICE_UUIDS_EXT        = 0x9596;
    constexpr GLenum  GL_DEVICE_UUID_EXT             = 0x9597;
    constexpr GLenum  GL_DRIVER_UUID_EXT             = 0x9598;

    // --------------------------------------------------- GL entry-point table
    typedef void (*tl4t_glproc)(void);

    struct CEglStreamGlFns
    {
        // core-ish
        void   (*GetError)(void);
        GLenum (*GetErrorR)(void);
        const unsigned char *(*GetString)(GLenum);
        void   (*GetIntegerv)(GLenum, GLint *);
        void   (*GetIntegeri_v)(GLenum, GLuint, GLint *);
        void   (*GetUnsignedBytei_vEXT)(GLenum, GLuint, GLubyte *);
        void   (*GetUnsignedBytevEXT)(GLenum, GLubyte *);
        void   (*GenTextures)(GLsizei, GLuint *);
        void   (*DeleteTextures)(GLsizei, const GLuint *);
        void   (*BindTexture)(GLenum, GLuint);
        void   (*ActiveTexture)(GLenum);
        void   (*TexParameteri)(GLenum, GLenum, GLint);
        void   (*GenFramebuffers)(GLsizei, GLuint *);
        void   (*DeleteFramebuffers)(GLsizei, const GLuint *);
        void   (*GenRenderbuffers)(GLsizei, GLuint *);
        void   (*DeleteRenderbuffers)(GLsizei, const GLuint *);
        void   (*BindRenderbuffer)(GLenum, GLuint);
        void   (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
        void   (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
        void   (*BindFramebuffer)(GLenum, GLuint);
        void   (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
        GLenum (*CheckFramebufferStatus)(GLenum);
        void   (*GenBuffers)(GLsizei, GLuint *);
        void   (*DeleteBuffers)(GLsizei, const GLuint *);
        void   (*BindBuffer)(GLenum, GLuint);
        void   (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
        void   (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
        void   (*CopyBufferSubData)(GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr);
        void   (*GenVertexArrays)(GLsizei, GLuint *);
        void   (*BindVertexArray)(GLuint);
        void   (*Enable)(GLenum);
        void   (*Disable)(GLenum);
        void   (*DrawArrays)(GLenum, GLint, GLsizei);
        void   (*Viewport)(GLint, GLint, GLsizei, GLsizei);
        void   (*Flush)(void);
        void *(*FenceSync)(GLenum, GLbitfield);
        GLenum (*ClientWaitSync)(void *, GLbitfield, uint64_t);
        void (*DeleteSync)(void *);

        void   (*Finish)(void);
        GLuint (*CreateShader)(GLenum);
        void   (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
        void   (*CompileShader)(GLuint);
        void   (*GetShaderiv)(GLuint, GLenum, GLint *);
        void   (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
        GLuint (*CreateProgram)(void);
        void   (*AttachShader)(GLuint, GLuint);
        void   (*TransformFeedbackVaryings)(GLuint, GLsizei, const GLchar *const *, GLenum);
        void   (*LinkProgram)(GLuint);
        void   (*GetProgramiv)(GLuint, GLenum, GLint *);
        void   (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
        void   (*UseProgram)(GLuint);
        GLint  (*GetUniformLocation)(GLuint, const GLchar *);
        void   (*Uniform1i)(GLint, GLint);
        void   (*BindBufferBase)(GLenum, GLuint, GLuint);
        void   (*BeginTransformFeedback)(GLenum);
        void   (*EndTransformFeedback)(void);
        // EXT external objects / semaphores
        void   (*CreateMemoryObjectsEXT)(GLsizei, GLuint *);
        void   (*DeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
        void   (*MemoryObjectParameterivEXT)(GLuint, GLenum, const GLint *);
        void   (*ImportMemoryFdEXT)(GLuint, GLsizeiptr, GLenum, GLint);
        void   (*BufferStorageMemEXT)(GLenum, GLsizeiptr, GLuint, GLintptr);
        void   (*GenSemaphoresEXT)(GLsizei, GLuint *);
        GLboolean (*IsSemaphoreEXT)(GLuint);
        void   (*DeleteSemaphoresEXT)(GLsizei, const GLuint *);
        void   (*ImportSemaphoreFdEXT)(GLuint, GLenum, GLint);
        void   (*SignalSemaphoreEXT)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
        void   (*WaitSemaphoreEXT)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
    };

    // ------------------------------------------------------- EGL entry points
    struct CEglStreamEglFns
    {
        PFNEGLCREATESTREAMFROMFILEDESCRIPTORKHRPROC   CreateStreamFromFd;
        PFNEGLDESTROYSTREAMKHRPROC                    DestroyStream;
        PFNEGLSTREAMATTRIBKHRPROC                     StreamAttrib;
        PFNEGLQUERYSTREAMKHRPROC                      QueryStream;
        PFNEGLSTREAMCONSUMERGLTEXTUREEXTERNALKHRPROC  ConsumerGLTextureExternal;
        PFNEGLSTREAMCONSUMERACQUIREKHRPROC            ConsumerAcquire;
        PFNEGLSTREAMCONSUMERRELEASEKHRPROC            ConsumerRelease;
    };

    // ------------------------------------------------------------------ records
    struct CEglStreamRecord;

    // ------------------------------------------------------------------- thread
    static std::thread              s_Thread;
    static std::mutex               s_QMtx;
    static std::condition_variable  s_QCv;
    static std::deque<std::shared_ptr<struct SJob>> s_Queue;
    static std::atomic<bool>        s_Quit{ false };
    static std::atomic<bool>        s_Started{ false };
    static std::atomic<bool>        s_EglOk{ false };

    static EGLDisplay               s_Dpy = EGL_NO_DISPLAY;
    static EGLConfig                s_Cfg = nullptr;
    static EGLContext               s_Ctx = EGL_NO_CONTEXT;
    static CEglStreamEglFns         s_E;
    static CEglStreamGlFns          s_G;
    static bool                     s_HaveGl = false;
    static GLuint                   s_Prog = 0;
    static GLuint                   s_Vao = 0;
    static GLint                    s_uTex = -1, s_uW = -1, s_uH = -1, s_uFlip = -1;
    static int                      s_AcquireTimeoutMs = 2000;

    // M9Y P1: owner-side deferred finalization. The consumer thread never frees
    // a record; it hands detached generations to this list and signals the
    // eventfd, drained on the wlserver event loop (the same context the
    // previous inline dispose ran in).
    static std::mutex                s_RetireMtx;
    static std::vector<CEglStreamRecord *> s_RetirePending;
    static int                       s_RetireFd = -1;
    static struct wl_event_source   *s_RetireSource = nullptr;

    struct SJob
    {
        enum EKind { CONNECT, FRAME, CONSUME, TEARDOWN } kind = CONNECT;
        CEglStreamRecord *rec = nullptr;
        int      fd = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        int      flipY = 0;
        TegraL4TSlotImportInfo slot{};
        bool     detach = false;             // TEARDOWN: record handed to owner
        bool     ok = false;
        bool     done = false;
        std::mutex m;
        std::condition_variable cv;
    };

    // One EGLStream "generation" == one Xwayland wl_buffer (the stream) and,
    // once attached, one transport slot + consumer GL objects.
    struct CEglStreamRecord
    {
        ::wlr_buffer base;                  // must stay first-ish for the cast path
        ::wl_resource *buf_res = nullptr;
        ::wl_resource *surface_res = nullptr;
        ::wl_resource *surface_destroy_listener_added = nullptr;
        wl_listener     surface_destroy_listener{};

        int      stream_fd = -1;             // owned until CONNECT consumed it
        uint32_t width = 0, height = 0;
        int      flipY = 0;

        EGLint   connect_error = EGL_SUCCESS;
        bool     retired = false;
        bool     quiescent = false;
        std::atomic<bool> retiring{ false };        // M9Y P1: destroy started; refuse new jobs
        std::atomic<bool> destroy_started{ false }; // M9Y P1: one-shot destroy guard
        std::atomic<int>  owner_pins{ 0 };          // M9Y P1: real copy/ack in flight (owner)
        bool     attached = false;
        bool     connected = false;
        bool     destroyed = false;
        uint64_t commit_serial = 0;

        CTegraL4TExportedSlot *slot = nullptr;

        // consumer-thread-owned GL objects (owner thread only)
        EGLStreamKHR stream = EGL_NO_STREAM_KHR;
        GLuint tex = 0, fbo = 0, rbo = 0, mobj = 0, extbuf = 0, tfbuf = 0;
        GLuint sReady = 0, sReturned = 0;
        uint64_t frames_written = 0;
    };

    static void EglStreamBufferDestroyImpl( ::wlr_buffer *pBuf );
    static const ::wlr_buffer_impl s_EglStreamBufferImpl = { .destroy = EglStreamBufferDestroyImpl };

    // M9Y P1: job ownership lives in the queue (shared_ptr). A submitter that
    // times out drops its reference; the worker still owns and completes the
    // job. The retirement guard is checked under the same queue lock that
    // pushes, so no FRAME/CONSUME job can be enqueued after a TEARDOWN.
    enum ESubmitResult { SUBMITTED_DONE, SUBMITTED_TIMEOUT, SUBMIT_REFUSED };

    static ESubmitResult SubmitJobSync( const std::shared_ptr<SJob> &pJob, int nTimeoutMs )
    {
        const auto tWait0 = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk( s_QMtx );
            if ( pJob->kind != SJob::TEARDOWN && pJob->rec != nullptr && pJob->rec->retiring.load() )
                return SUBMIT_REFUSED;
            s_Queue.push_back( pJob );
        }
        s_QCv.notify_one();
        std::unique_lock<std::mutex> lk( pJob->m );
        const bool bDone = pJob->cv.wait_for( lk, std::chrono::milliseconds( nTimeoutMs ), [&]{ return pJob->done; } );
        const auto uWaitedMs = (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tWait0 ).count();
        if ( !bDone || uWaitedMs >= 1000ull )
            tl4t_es_log( "job-wait: kind=%d timeout_ms=%d waited_ms=%llu done=%d",
                         (int)pJob->kind, nTimeoutMs, uWaitedMs, (int)bDone );
        return bDone ? SUBMITTED_DONE : SUBMITTED_TIMEOUT;
    }

    static void SubmitJobTeardown( CEglStreamRecord *rec )
    {
        auto pJob = std::make_shared<SJob>();
        pJob->kind = SJob::TEARDOWN;
        pJob->rec = rec;
        pJob->detach = true;
        {
            std::lock_guard<std::mutex> lk( s_QMtx );
            s_Queue.push_back( pJob );   // TEARDOWN bypasses the retirement guard
        }
        s_QCv.notify_one();
    }

    // ---------------------------------------------------------- external texture
    static bool GlCheckNoError( const char *pStage )
    {
        if ( s_G.GetErrorR == nullptr ) return true;
        GLenum e = s_G.GetErrorR();
        if ( e != GL_NO_ERROR ) { tl4t_es_log( "GL error 0x%04x at %s", (unsigned)e, pStage ); return false; }
        return true;
    }

    static bool CreateSharedGlObjects()
    {
        static const char *vs =
            "#version 300 es\n"
            "#extension GL_OES_EGL_image_external_essl3 : require\n"
            "precision highp float;\n"
            "precision highp int;\n"
            "uniform samplerExternalOES uTex;\n"
            "uniform int uW;\n"
            "uniform int uH;\n"
            "uniform int uFlip;\n"
            "flat out uint vWord;\n"
            "void main(){\n"
            "  int id = gl_VertexID;\n"
            "  int x = id % uW;\n"
            "  int y = id / uW;\n"
            "  float sy = uFlip != 0 ? (float(uH - 1 - y) + 0.5) / float(uH)\n"
            "                        : (float(y) + 0.5) / float(uH);\n"
            "  vec2 tc = vec2((float(x) + 0.5) / float(uW), sy);\n"
            "  vec4 c = clamp(texture(uTex, tc), 0.0, 1.0);\n"
            "  uvec4 b = uvec4(c * 255.0 + 0.5);\n"
            "  vWord = b.r | (b.g << 8u) | (b.b << 16u) | (b.a << 24u);\n"
            "  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n"
            "}\n";
        static const char *fs =
            "#version 300 es\n"
            "precision mediump float;\n"
            "out vec4 fragColor;\n"
            "void main(){ fragColor = vec4(0.0); }\n";

        GLuint vsh = s_G.CreateShader( GL_VERTEX_SHADER );
        const GLchar *vsSrc = vs;
        s_G.ShaderSource( vsh, 1, &vsSrc, nullptr );
        s_G.CompileShader( vsh );
        GLint ok = 0; s_G.GetShaderiv( vsh, GL_COMPILE_STATUS, &ok );
        if ( !ok ) { tl4t_es_log( "vertex shader compile failed" ); return false; }
        GLuint fsh = s_G.CreateShader( GL_FRAGMENT_SHADER );
        const GLchar *fsSrc = fs;
        s_G.ShaderSource( fsh, 1, &fsSrc, nullptr );
        s_G.CompileShader( fsh );
        s_G.GetShaderiv( fsh, GL_COMPILE_STATUS, &ok );
        if ( !ok ) { tl4t_es_log( "fragment shader compile failed" ); return false; }

        s_Prog = s_G.CreateProgram();
        s_G.AttachShader( s_Prog, vsh );
        s_G.AttachShader( s_Prog, fsh );
        const GLchar *varyings[1] = { "vWord" };
        s_G.TransformFeedbackVaryings( s_Prog, 1, varyings, GL_INTERLEAVED_ATTRIBS );
        s_G.LinkProgram( s_Prog );
        s_G.GetProgramiv( s_Prog, GL_LINK_STATUS, &ok );
        if ( !ok ) { tl4t_es_log( "program link failed" ); return false; }

        s_uTex = s_G.GetUniformLocation( s_Prog, "uTex" );
        s_uW   = s_G.GetUniformLocation( s_Prog, "uW" );
        s_uH   = s_G.GetUniformLocation( s_Prog, "uH" );
        s_uFlip = s_G.GetUniformLocation( s_Prog, "uFlip" );

        s_G.GenVertexArrays( 1, &s_Vao );
        return true;
    }

    static void ResolveGl()
    {
        auto gp = []( const char *n ) -> tl4t_glproc { return eglGetProcAddress( n ); };
#define TL4T_GPF(field, name) do { \
            tl4t_glproc p_ = gp(name); \
            if ( p_ ) memcpy( &s_G.field, &p_, sizeof(p_) ); else s_G.field = nullptr; } while (0)
        TL4T_GPF( GetError, "glGetError" );
        TL4T_GPF( GetErrorR, "glGetError" );
        TL4T_GPF( GetString, "glGetString" );
        TL4T_GPF( GetIntegerv, "glGetIntegerv" );
        TL4T_GPF( GetIntegeri_v, "glGetIntegeri_v" );
        TL4T_GPF( GetUnsignedBytei_vEXT, "glGetUnsignedBytei_vEXT" );
        TL4T_GPF( GetUnsignedBytevEXT, "glGetUnsignedBytevEXT" );
        TL4T_GPF( GenTextures, "glGenTextures" );
        TL4T_GPF( DeleteTextures, "glDeleteTextures" );
        TL4T_GPF( BindTexture, "glBindTexture" );
        TL4T_GPF( ActiveTexture, "glActiveTexture" );
        TL4T_GPF( TexParameteri, "glTexParameteri" );
        TL4T_GPF( GenFramebuffers, "glGenFramebuffers" );
        TL4T_GPF( DeleteFramebuffers, "glDeleteFramebuffers" );
        TL4T_GPF( GenRenderbuffers, "glGenRenderbuffers" );
        TL4T_GPF( DeleteRenderbuffers, "glDeleteRenderbuffers" );
        TL4T_GPF( BindRenderbuffer, "glBindRenderbuffer" );
        TL4T_GPF( RenderbufferStorage, "glRenderbufferStorage" );
        TL4T_GPF( FramebufferRenderbuffer, "glFramebufferRenderbuffer" );
        TL4T_GPF( BindFramebuffer, "glBindFramebuffer" );
        TL4T_GPF( FramebufferTexture2D, "glFramebufferTexture2D" );
        TL4T_GPF( CheckFramebufferStatus, "glCheckFramebufferStatus" );
        TL4T_GPF( GenBuffers, "glGenBuffers" );
        TL4T_GPF( DeleteBuffers, "glDeleteBuffers" );
        TL4T_GPF( BindBuffer, "glBindBuffer" );
        TL4T_GPF( BufferData, "glBufferData" );
        TL4T_GPF( BufferSubData, "glBufferSubData" );
        TL4T_GPF( CopyBufferSubData, "glCopyBufferSubData" );
        TL4T_GPF( GenVertexArrays, "glGenVertexArrays" );
        TL4T_GPF( BindVertexArray, "glBindVertexArray" );
        TL4T_GPF( Enable, "glEnable" );
        TL4T_GPF( Disable, "glDisable" );
        TL4T_GPF( DrawArrays, "glDrawArrays" );
        TL4T_GPF( Viewport, "glViewport" );
        TL4T_GPF( Flush, "glFlush" );
        TL4T_GPF( FenceSync, "glFenceSync" );
        TL4T_GPF( ClientWaitSync, "glClientWaitSync" );
        TL4T_GPF( DeleteSync, "glDeleteSync" );
        TL4T_GPF( Finish, "glFinish" );
        TL4T_GPF( CreateShader, "glCreateShader" );
        TL4T_GPF( ShaderSource, "glShaderSource" );
        TL4T_GPF( CompileShader, "glCompileShader" );
        TL4T_GPF( GetShaderiv, "glGetShaderiv" );
        TL4T_GPF( GetShaderInfoLog, "glGetShaderInfoLog" );
        TL4T_GPF( CreateProgram, "glCreateProgram" );
        TL4T_GPF( AttachShader, "glAttachShader" );
        TL4T_GPF( TransformFeedbackVaryings, "glTransformFeedbackVaryings" );
        TL4T_GPF( LinkProgram, "glLinkProgram" );
        TL4T_GPF( GetProgramiv, "glGetProgramiv" );
        TL4T_GPF( GetProgramInfoLog, "glGetProgramInfoLog" );
        TL4T_GPF( UseProgram, "glUseProgram" );
        TL4T_GPF( GetUniformLocation, "glGetUniformLocation" );
        TL4T_GPF( Uniform1i, "glUniform1i" );
        TL4T_GPF( BindBufferBase, "glBindBufferBase" );
        TL4T_GPF( BeginTransformFeedback, "glBeginTransformFeedback" );
        TL4T_GPF( EndTransformFeedback, "glEndTransformFeedback" );
        TL4T_GPF( CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT" );
        TL4T_GPF( DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT" );
        TL4T_GPF( MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT" );
        TL4T_GPF( ImportMemoryFdEXT, "glImportMemoryFdEXT" );
        TL4T_GPF( BufferStorageMemEXT, "glBufferStorageMemEXT" );
        TL4T_GPF( GenSemaphoresEXT, "glGenSemaphoresEXT" );
        TL4T_GPF( IsSemaphoreEXT, "glIsSemaphoreEXT" );
        TL4T_GPF( DeleteSemaphoresEXT, "glDeleteSemaphoresEXT" );
        TL4T_GPF( ImportSemaphoreFdEXT, "glImportSemaphoreFdEXT" );
        TL4T_GPF( SignalSemaphoreEXT, "glSignalSemaphoreEXT" );
        TL4T_GPF( WaitSemaphoreEXT, "glWaitSemaphoreEXT" );
#undef TL4T_GPF
    }

    static bool ResolveEgl()
    {
        auto ep = []( const char *n ) -> tl4t_glproc { return eglGetProcAddress( n ); };
#define TL4T_EPF(field, name) do { \
            tl4t_glproc p_ = ep(name); \
            if ( p_ ) memcpy( &s_E.field, &p_, sizeof(p_) ); else s_E.field = nullptr; } while (0)
        TL4T_EPF( CreateStreamFromFd, "eglCreateStreamFromFileDescriptorKHR" );
        TL4T_EPF( DestroyStream, "eglDestroyStreamKHR" );
        TL4T_EPF( StreamAttrib, "eglStreamAttribKHR" );
        TL4T_EPF( QueryStream, "eglQueryStreamKHR" );
        TL4T_EPF( ConsumerGLTextureExternal, "eglStreamConsumerGLTextureExternalKHR" );
        TL4T_EPF( ConsumerAcquire, "eglStreamConsumerAcquireKHR" );
        TL4T_EPF( ConsumerRelease, "eglStreamConsumerReleaseKHR" );
#undef TL4T_EPF
        return s_E.CreateStreamFromFd && s_E.DestroyStream && s_E.ConsumerGLTextureExternal &&
               s_E.ConsumerAcquire && s_E.ConsumerRelease;
    }

    // Consumer EGLDisplay/context on the dedicated thread. Standalone: EGL_NO_CONTEXT
    // share (decision 6 - Gamescope has no GL context to share with).
    static bool ConsumerEglInit()
    {
        tl4t_glproc pQdev = eglGetProcAddress( "eglQueryDevicesEXT" );
        tl4t_glproc pGpd  = eglGetProcAddress( "eglGetPlatformDisplayEXT" );
        PFNEGLQUERYDEVICESEXTPROC       qdev = nullptr;
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd  = nullptr;
        if ( pQdev ) memcpy( &qdev, &pQdev, sizeof(qdev) );
        if ( pGpd )  memcpy( &gpd,  &pGpd,  sizeof(gpd) );
        if ( !qdev || !gpd ) { tl4t_es_log( "EGL device entry points missing" ); return false; }

        EGLDeviceEXT devices[8]; EGLint ndev = 0;
        (void)eglGetError();
        if ( !qdev( 8, devices, &ndev ) || ndev <= 0 )
        { tl4t_es_log( "eglQueryDevicesEXT found no device" ); return false; }

        (void)eglGetError();
        s_Dpy = gpd( EGL_PLATFORM_DEVICE_EXT, devices[0], nullptr );
        if ( s_Dpy == EGL_NO_DISPLAY )
        { tl4t_es_log( "eglGetPlatformDisplayEXT failed" ); return false; }

        EGLint maj = 0, min = 0;
        if ( !eglInitialize( s_Dpy, &maj, &min ) )
        { tl4t_es_log( "eglInitialize failed (0x%04x)", (unsigned)eglGetError() ); return false; }

        const char *exts = eglQueryString( s_Dpy, EGL_EXTENSIONS );
        auto has = [&]( const char *w ) {
            if ( !exts ) return false;
            size_t wl = strlen(w);
            const char *p = exts;
            while ( ( p = strstr( p, w ) ) ) {
                if ( ( p == exts || p[-1] == ' ' ) && ( p[wl] == '\0' || p[wl] == ' ' ) ) return true;
                p += wl;
            }
            return false;
        };
        if ( !has( "EGL_KHR_stream_consumer_gltexture" ) )
        { tl4t_es_log( "EGL_KHR_stream_consumer_gltexture missing" ); return false; }

        const EGLint cfgAttr[] = {
            EGL_SURFACE_TYPE, EGL_STREAM_BIT_KHR | EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE };
        EGLint n = 0;
        (void)eglGetError();
        if ( !eglChooseConfig( s_Dpy, cfgAttr, &s_Cfg, 1, &n ) || n < 1 )
        { tl4t_es_log( "eglChooseConfig(EGL_STREAM_BIT_KHR) failed" ); return false; }

        if ( !eglBindAPI( EGL_OPENGL_ES_API ) ) { tl4t_es_log( "eglBindAPI failed" ); return false; }
        const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
        for ( int i = 0; i < 3 && s_Ctx == EGL_NO_CONTEXT; i++ )
        { (void)eglGetError(); s_Ctx = eglCreateContext( s_Dpy, s_Cfg, EGL_NO_CONTEXT, ctxAttr ); }
        if ( s_Ctx == EGL_NO_CONTEXT ) { tl4t_es_log( "eglCreateContext failed" ); return false; }

        (void)eglGetError();
        if ( !eglMakeCurrent( s_Dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_Ctx ) )
        { tl4t_es_log( "eglMakeCurrent(surfaceless) failed" ); return false; }

        ResolveGl();
        if ( !ResolveEgl() ) { tl4t_es_log( "EGL stream entry points missing" ); return false; }
        if ( !s_G.CreateMemoryObjectsEXT || !s_G.ImportMemoryFdEXT || !s_G.BufferStorageMemEXT ||
             !s_G.GenSemaphoresEXT || !s_G.ImportSemaphoreFdEXT || !s_G.SignalSemaphoreEXT ||
             !s_G.WaitSemaphoreEXT || !s_G.GenTextures || !s_G.DrawArrays )
        { tl4t_es_log( "required GL external-object/semaphore entry points missing" ); return false; }

        if ( !CreateSharedGlObjects() ) { tl4t_es_log( "shared GL program setup failed" ); return false; }
        s_HaveGl = true;
        tl4t_es_log( "consumer EGL/GL ready (EGL %d.%d, surfaceless)", (int)maj, (int)min );
        return true;
    }

    // -------------------------------------------------------- per-generation GL
    static bool GlImportSlotIntoRecord( CEglStreamRecord *rec, const TegraL4TSlotImportInfo &info )
    {
        // Cross-API device UUID check (fail-closed), mirroring gl_match_devices.
        // Per EXT_external_objects 4.2.1 the device UUID is a UUID_SIZE_EXT (16)
        // ubyte array read via glGetUnsignedBytei_vEXT, and the driver UUID via
        // glGetUnsignedBytevEXT. Both must equal the VK slot UUIDs, else refuse.
        if ( !s_G.GetIntegerv || !s_G.GetUnsignedBytei_vEXT || !s_G.GetUnsignedBytevEXT )
        { tl4t_es_log( "UUID query entry points missing (glGetUnsignedBytei_vEXT/glGetUnsignedBytevEXT)" ); return false; }

        // Drain any pre-existing GL errors so the checks below are meaningful.
        if ( s_G.GetErrorR ) while ( s_G.GetErrorR() != GL_NO_ERROR ) {}

        GLint n = 0;
        s_G.GetIntegerv( GL_NUM_DEVICE_UUIDS_EXT, &n );
        if ( !GlCheckNoError( "glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT)" ) ) return false;
        if ( n < 1 )
        { tl4t_es_log( "no GL device UUIDs (NUM_DEVICE_UUIDS_EXT=%d)", (int)n ); return false; }

        GLubyte glu[16] = {0}, drv[16] = {0};
        s_G.GetUnsignedBytei_vEXT( GL_DEVICE_UUID_EXT, 0, glu );
        if ( !GlCheckNoError( "glGetUnsignedBytei_vEXT(GL_DEVICE_UUID_EXT)" ) ) return false;
        s_G.GetUnsignedBytevEXT( GL_DRIVER_UUID_EXT, drv );
        if ( !GlCheckNoError( "glGetUnsignedBytevEXT(GL_DRIVER_UUID_EXT)" ) ) return false;

        if ( memcmp( glu, info.device_uuid, 16 ) != 0 )
        { tl4t_es_log( "device UUID mismatch (GL vs Vulkan slot)" ); return false; }
        if ( memcmp( drv, info.driver_uuid, 16 ) != 0 )
        { tl4t_es_log( "driver UUID mismatch (GL vs Vulkan slot)" ); return false; }

        s_G.CreateMemoryObjectsEXT( 1, &rec->mobj );
        if ( !rec->mobj ) { tl4t_es_log( "glCreateMemoryObjectsEXT failed" ); return false; }
        if ( info.dedicated )
        {
            const GLint one = 1;
            s_G.MemoryObjectParameterivEXT( rec->mobj, GL_DEDICATED_MEMORY_OBJECT_EXT, &one );
        }
        int mfd = dup( info.mem_fd );
        if ( mfd < 0 ) { tl4t_es_log( "dup(mem_fd) failed" ); return false; }
        s_G.ImportMemoryFdEXT( rec->mobj, (GLsizeiptr)info.allocation_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, mfd );
        if ( !GlCheckNoError( "glImportMemoryFdEXT" ) ) return false;

        s_G.GenBuffers( 1, &rec->extbuf );
        s_G.BindBuffer( GL_ARRAY_BUFFER, rec->extbuf );
        s_G.BufferStorageMemEXT( GL_ARRAY_BUFFER, (GLsizeiptr)info.allocation_size, rec->mobj, 0 );
        if ( !GlCheckNoError( "glBufferStorageMemEXT" ) ) return false;

        s_G.GenBuffers( 1, &rec->tfbuf );
        s_G.BindBuffer( GL_ARRAY_BUFFER, rec->tfbuf );
        s_G.BufferData( GL_ARRAY_BUFFER, (GLsizeiptr)info.allocation_size, nullptr, 0x88E0 /*GL_STREAM_DRAW*/ );
        if ( !GlCheckNoError( "glBufferData(tfbuf)" ) ) return false;
        s_G.BindBuffer( GL_ARRAY_BUFFER, 0 );

        int rfd = dup( info.ready_fd );
        int tfd = dup( info.returned_fd );
        if ( rfd < 0 || tfd < 0 ) { tl4t_es_log( "dup(sem fd) failed" ); return false; }
        s_G.GenSemaphoresEXT( 2, &rec->sReady );  // sReady, sReturned are consecutive
        s_G.ImportSemaphoreFdEXT( rec->sReady, GL_HANDLE_TYPE_OPAQUE_FD_EXT, rfd );
        s_G.ImportSemaphoreFdEXT( rec->sReturned, GL_HANDLE_TYPE_OPAQUE_FD_EXT, tfd );
        if ( !GlCheckNoError( "glImportSemaphoreFdEXT" ) ) return false;
        if ( !s_G.IsSemaphoreEXT( rec->sReady ) || !s_G.IsSemaphoreEXT( rec->sReturned ) )
        { tl4t_es_log( "imported semaphores not valid" ); return false; }

        return true;
    }


    // A failed FIRST import created no stream, GL objects or GPU work. Reject
    // only this exact 1x1/BAD_STATE case. It is not a successful frame and is
    // never retried; all other errors and all job timeouts keep fail-closed.
    static bool M9sCanRejectUnimportable( uint32_t width, uint32_t height, EGLint error,
                                 EGLStreamKHR stream, bool connected )
    {
        return width > 0 && height > 0 && error == EGL_BAD_STATE_KHR &&
               stream == EGL_NO_STREAM_KHR && !connected;
    }
    static bool M9sRetirementEnabled()
    {
        static bool enabled = getenv("GAMESCOPE_M9S_STREAM_RETIRE") &&
            !strcmp(getenv("GAMESCOPE_M9S_STREAM_RETIRE"), "1");
        return enabled;
    }

    static bool M9sRetireFailedConnect( CEglStreamRecord *rec, EGLint error );

    static bool GlConnectRecord( CEglStreamRecord *rec, int fd, uint32_t w, uint32_t h,
                                 const TegraL4TSlotImportInfo &info )
    {
        // eglCreateStreamFromFileDescriptorKHR consumes a dup; we own fd here.
        rec->stream = s_E.CreateStreamFromFd( s_Dpy, (EGLNativeFileDescriptorKHR)fd );
        if ( rec->stream == EGL_NO_STREAM_KHR )
        {
            rec->connect_error = eglGetError();
            if ( M9sRetirementEnabled() && M9sCanRejectUnimportable( w, h, rec->connect_error, rec->stream, rec->connected ) )
                tl4t_es_log( "M9S_IMPORT_REFUSED error=0x%04x width=%u height=%u stream_created=0 gpu_submitted=0",
                    (unsigned)rec->connect_error, w, h );
            else
                tl4t_es_log( "eglCreateStreamFromFileDescriptorKHR failed (0x%04x)", (unsigned)rec->connect_error );
            return false;
        }

        // 2 s acquire bound (decision 8): never stall the compositor.
        if ( s_E.StreamAttrib )
            s_E.StreamAttrib( s_Dpy, rec->stream, EGL_CONSUMER_ACQUIRE_TIMEOUT_USEC_KHR, s_AcquireTimeoutMs * 1000 );
        (void)eglGetError();

        s_G.GenTextures( 1, &rec->tex );
        s_G.BindTexture( GL_TEXTURE_EXTERNAL_OES, rec->tex );
        s_G.TexParameteri( GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
        s_G.TexParameteri( GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
        s_G.TexParameteri( GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
        s_G.TexParameteri( GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

        if ( !s_E.ConsumerGLTextureExternal( s_Dpy, rec->stream ) )
        {
            rec->connect_error = eglGetError();
            if ( !M9sRetireFailedConnect( rec, rec->connect_error ) )
                tl4t_es_log( "eglStreamConsumerGLTextureExternalKHR failed (0x%04x)", (unsigned)rec->connect_error );
            return false;
        }

        // Complete capture FBO (proven stub shape, run #23). The external
        // texture's attributes are undefined until the stream is associated with
        // a producer, so attaching it as COLOR_ATTACHMENT0 yields
        // GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT (0x8CD7). Attach a complete
        // RGBA8 renderbuffer instead; writes are irrelevant under
        // GL_RASTERIZER_DISCARD, only completeness matters.
        s_G.GenFramebuffers( 1, &rec->fbo );
        s_G.GenRenderbuffers( 1, &rec->rbo );
        s_G.BindRenderbuffer( GL_RENDERBUFFER, rec->rbo );
        s_G.RenderbufferStorage( GL_RENDERBUFFER, GL_RGBA8, 32, 32 );
        s_G.BindFramebuffer( GL_FRAMEBUFFER, rec->fbo );
        s_G.FramebufferRenderbuffer( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rec->rbo );
        GLenum st = s_G.CheckFramebufferStatus( GL_FRAMEBUFFER );
        s_G.BindFramebuffer( GL_FRAMEBUFFER, 0 );
        if ( st != GL_FRAMEBUFFER_COMPLETE )
        { tl4t_es_log( "capture FBO incomplete (0x%04x)", (unsigned)st ); return false; }

        if ( !GlImportSlotIntoRecord( rec, info ) ) return false;

        (void)w; (void)h;
        return true;
    }

    // Acquire one frame, transfer it FULL EXTENT into the slot buffer and signal
    // `ready`. No CPU readback: transform feedback writes the GPU buffer directly.

    // Querying DISCONNECTED is positive evidence of an endpoint lifecycle end,
    // not an interpretation of error 0x321c. Flush + bounded GL sync includes
    // prior returned-slot waits and any current capture/copy commands. Only a
    // successful fence permits retirement; timeout/error is still fatal STOP.
    static bool M9sRetireDisconnected( CEglStreamRecord *rec, const char *stage, EGLint cause )
    {
        if ( !M9sRetirementEnabled() ) return false;
        if ( !s_E.QueryStream || !s_G.FenceSync || !s_G.ClientWaitSync || !s_G.DeleteSync )
            tl4t_es_fail_closed( "stream retirement prerequisites missing" );
        EGLint state = 0; (void)eglGetError();
        const EGLBoolean ok = s_E.QueryStream( s_Dpy, rec->stream, EGL_STREAM_STATE_KHR, &state );
        const EGLint error = eglGetError();
        if ( !ok )
        {
            tl4t_es_log( "M9S_STREAM_STATE_QUERY_ERROR stage=%s error=0x%x", stage, error );
            tl4t_es_fail_closed( "stream retirement state query failed" );
        }
        if ( state != EGL_STREAM_STATE_DISCONNECTED_KHR )
        {
            if ( strcmp(stage, "before-acquire") )
                tl4t_es_log( "M9S_STREAM_NOT_RETIRED stage=%s cause=0x%x state=0x%x w=%u h=%u frames=%llu",
                    stage, cause, state, rec->width, rec->height, (unsigned long long)rec->frames_written );
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        void *fence = s_G.FenceSync( 0x9117 /* GPU_COMMANDS_COMPLETE */, 0 );
        if ( !fence || !GlCheckNoError( "retirement fence creation" ) )
            tl4t_es_fail_closed( "stream retirement fence creation failed" );
        s_G.Flush();
        const GLenum wait = s_G.ClientWaitSync( fence, 0, 2000000000ull );
        if ( wait != 0x911a /* ALREADY_SIGNALED */ && wait != 0x911c /* CONDITION_SATISFIED */ )
        {
            tl4t_es_log( "M9S_STREAM_RETIRE_NOT_QUIESCENT stage=%s wait=0x%x", stage, wait );
            tl4t_es_fail_closed( "stream retirement GPU fence failed or timed out" );
        }
        s_G.DeleteSync( fence );
        if ( !GlCheckNoError( "stream retirement GPU completion" ) )
            tl4t_es_fail_closed( "stream retirement GPU completion error" );
        rec->retired = true; rec->quiescent = true;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        tl4t_es_log( "M9S_STREAM_RETIRED stage=%s cause=0x%x state=0x%x fence=0x%x waited_ms=%lld w=%u h=%u frames=%llu texture_delivered=0",
            stage, cause, state, wait, (long long)ms, rec->width, rec->height, (unsigned long long)rec->frames_written );
        return true;
    }


    static bool M9sRetireFailedConnect( CEglStreamRecord *rec, EGLint error )
    {
        if ( error != EGL_BAD_STATE_KHR || !M9sRetirementEnabled() ) return false;
        if ( !M9sRetireDisconnected( rec, "connect", error ) ) return false;
        // This stage precedes FBO/transport memory/semaphore import. Drain the
        // context first, then dispose its partial texture/stream on this owner.
        // Failure to destroy is an unhandled cleanup error, never a refusal PASS.
        if ( rec->tex )
        {
            s_G.DeleteTextures( 1, &rec->tex ); rec->tex = 0;
            if ( !GlCheckNoError( "retire partial connect texture" ) )
                tl4t_es_fail_closed( "partial stream connect GL cleanup failed" );
        }
        (void)eglGetError();
        const EGLBoolean destroyed = s_E.DestroyStream( s_Dpy, rec->stream );
        const EGLint destroyError = eglGetError();
        if ( !destroyed )
        {
            tl4t_es_log( "M9S_PARTIAL_CONNECT_DESTROY_ERROR error=0x%x", destroyError );
            tl4t_es_fail_closed( "partial stream connect EGL cleanup failed" );
        }
        rec->stream = EGL_NO_STREAM_KHR;
        tl4t_es_log( "M9S_PARTIAL_CONNECT_RETIRED cause=0x%x texture_deleted=1 stream_destroyed=1 gpu_quiescent=1 w=%u h=%u",
            error, rec->width, rec->height );
        return true;
    }

    static bool GlConsumeFrame( CEglStreamRecord *rec, int nFrameFlipY )
    {
        if ( rec->stream == EGL_NO_STREAM_KHR || rec->slot == nullptr )
        { tl4t_es_log( "consume: no stream/slot" ); return false; }

        // Wait until the compositor returned the slot from the previous frame.
        if ( rec->frames_written > 0 )
        {
            s_G.WaitSemaphoreEXT( rec->sReturned, 1, &rec->extbuf, 0, nullptr, nullptr );
            if ( !GlCheckNoError( "glWaitSemaphoreEXT(returned)" ) ) return false;
        }

        if ( M9sRetireDisconnected( rec, "before-acquire", EGL_SUCCESS ) ) return false;
        (void)eglGetError();
        if ( !s_E.ConsumerAcquire( s_Dpy, rec->stream ) )
        {
            const EGLint error = eglGetError();
            if ( error == EGL_BAD_STATE_KHR && M9sRetireDisconnected( rec, "acquire", error ) ) return false;
            tl4t_es_log( "eglStreamConsumerAcquireKHR failed/timeout within %dms error=0x%x", s_AcquireTimeoutMs, error );
            return false;
        }

        // M9N: query only after successful acquire, when both endpoints are
        // connected. Observation only: do not rewrite protocol orientation.
        if ( rec->frames_written == 0 && nFrameFlipY != (rec->flipY ? 1 : 0) && s_E.QueryStream != nullptr )
        {
            EGLint ox = EGL_DONT_CARE, oy = EGL_DONT_CARE, automatic = EGL_DONT_CARE;
            (void)eglGetError();
            const EGLBoolean xok = s_E.QueryStream( s_Dpy, rec->stream, 0x3366 /* EGL_STREAM_FRAME_ORIGIN_X_NV */, &ox );
            const EGLint xe = eglGetError();
            const EGLBoolean yok = s_E.QueryStream( s_Dpy, rec->stream, 0x3367 /* EGL_STREAM_FRAME_ORIGIN_Y_NV */, &oy );
            const EGLint ye = eglGetError();
            const EGLBoolean aok = s_E.QueryStream( s_Dpy, rec->stream, 0x3369 /* EGL_CONSUMER_AUTO_ORIENTATION_NV */, &automatic );
            const EGLint ae = eglGetError();
            tl4t_es_log( "m9n-origin: x_ok=%d x=0x%x x_err=0x%x y_ok=%d y=0x%x y_err=0x%x auto_ok=%d auto=%d auto_err=0x%x",
                         xok, ox, xe, yok, oy, ye, aok, automatic, ae );
        }

        const uint32_t w = rec->width, h = rec->height;
        s_G.UseProgram( s_Prog );
        s_G.Uniform1i( s_uTex, 0 );
        s_G.Uniform1i( s_uW, (GLint)w );
        s_G.Uniform1i( s_uH, (GLint)h );
        s_G.Uniform1i( s_uFlip, nFrameFlipY ? 1 : 0 );
        s_G.ActiveTexture( GL_TEXTURE0 );
        s_G.BindTexture( GL_TEXTURE_EXTERNAL_OES, rec->tex );
        s_G.BindVertexArray( s_Vao );
        s_G.BindBufferBase( GL_TRANSFORM_FEEDBACK_BUFFER, 0, rec->tfbuf );
        s_G.BindFramebuffer( GL_FRAMEBUFFER, rec->fbo );
        s_G.Enable( GL_RASTERIZER_DISCARD );
        s_G.BeginTransformFeedback( GL_POINTS );
        s_G.DrawArrays( GL_POINTS, 0, (GLsizei)( (uint64_t)w * h ) );
        s_G.EndTransformFeedback();
        s_G.Disable( GL_RASTERIZER_DISCARD );
        s_G.BindFramebuffer( GL_FRAMEBUFFER, 0 );
        if ( !GlCheckNoError( "transform feedback capture" ) )
        { s_E.ConsumerRelease( s_Dpy, rec->stream ); return false; }

        s_G.BindBuffer( GL_COPY_READ_BUFFER, rec->tfbuf );
        s_G.BindBuffer( GL_COPY_WRITE_BUFFER, rec->extbuf );
        s_G.CopyBufferSubData( GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, (GLsizeiptr)( (uint64_t)w * h * 4 ) );
        if ( !GlCheckNoError( "glCopyBufferSubData(tf->slot)" ) )
        { s_E.ConsumerRelease( s_Dpy, rec->stream ); return false; }

        (void)eglGetError();
        if ( !s_E.ConsumerRelease( s_Dpy, rec->stream ) )
        {
            const EGLint error = eglGetError();
            if ( error == EGL_BAD_STATE_KHR && M9sRetireDisconnected( rec, "release", error ) ) return false;
            tl4t_es_log( "eglStreamConsumerReleaseKHR failed (0x%04x)", (unsigned)error );
            return false;
        }

        s_G.SignalSemaphoreEXT( rec->sReady, 1, &rec->extbuf, 0, nullptr, nullptr );
        if ( !GlCheckNoError( "glSignalSemaphoreEXT(ready)" ) ) return false;
        s_G.Flush();
        if ( !GlCheckNoError( "glFlush" ) ) return false;

        rec->frames_written++;
        return true;
    }

    // Consume + release only (no capture): keeps the stream advancing for a frame
    // we will not display.
    static bool GlConsumeDiscard( CEglStreamRecord *rec )
    {
        if ( rec->stream == EGL_NO_STREAM_KHR ) return false;
        // A stale Xwayland commit does not imply a fresh EGLStream image.
        // This job is discard-only: never stall the compositor waiting for an
        // image it will not display. An image arriving after an OLD/EMPTY
        // observation remains queued for a later real producer commit.
        static const bool nonblockingDiscard = [] {
            const char *v = getenv("GAMESCOPE_TEGRA_EGLS_DISCARD_IDLE");
            return v && !strcmp(v, "1");
        }();
        if ( nonblockingDiscard )
        {
            if ( !s_E.QueryStream ) return false;
            EGLint state = 0; (void)eglGetError();
            const EGLBoolean queried = s_E.QueryStream(s_Dpy, rec->stream, EGL_STREAM_STATE_KHR, &state);
            const EGLint error = eglGetError();
            if ( !queried )
            {
                tl4t_es_log("M10O_DISCARD_QUERY_FAILED error=0x%x", error);
                return false;
            }
            if ( state == EGL_STREAM_STATE_OLD_FRAME_AVAILABLE_KHR || state == EGL_STREAM_STATE_EMPTY_KHR )
            {
                static uint64_t skipped = 0;
                if ( ++skipped <= 4 || skipped % 128 == 0 )
                    tl4t_es_log("M10O_DISCARD_IDLE state=0x%x skipped=%llu acquire=0 release=0",
                        state, (unsigned long long)skipped);
                return true;
            }
        }
        (void)eglGetError();
        if ( !s_E.ConsumerAcquire( s_Dpy, rec->stream ) )
        { tl4t_es_log( "discard acquire failed/timeout" ); return false; }
        (void)eglGetError();
        if ( !s_E.ConsumerRelease( s_Dpy, rec->stream ) )
        { tl4t_es_log( "discard release failed" ); return false; }
        return true;
    }

    // Destroy per-generation GL objects. Owner (consumer) thread only, after the
    // last acquire release and the copy fence retired. (decision 7)
    static void GlDestroyRecord( CEglStreamRecord *rec )
    {
        if ( rec->sReady || rec->sReturned )
        {
            GLuint sems[2] = { rec->sReady, rec->sReturned };
            s_G.DeleteSemaphoresEXT( 2, sems );
            rec->sReady = rec->sReturned = 0;
        }
        if ( rec->tfbuf ) { s_G.DeleteBuffers( 1, &rec->tfbuf ); rec->tfbuf = 0; }
        if ( rec->extbuf ) { s_G.DeleteBuffers( 1, &rec->extbuf ); rec->extbuf = 0; }
        if ( rec->mobj ) { s_G.DeleteMemoryObjectsEXT( 1, &rec->mobj ); rec->mobj = 0; }
        if ( rec->fbo ) { s_G.DeleteFramebuffers( 1, &rec->fbo ); rec->fbo = 0; }
        if ( rec->rbo ) { s_G.DeleteRenderbuffers( 1, &rec->rbo ); rec->rbo = 0; }
        if ( rec->tex ) { s_G.DeleteTextures( 1, &rec->tex ); rec->tex = 0; }
        if ( rec->stream != EGL_NO_STREAM_KHR )
        { s_E.DestroyStream( s_Dpy, rec->stream ); rec->stream = EGL_NO_STREAM_KHR; }
    }

    // ------------------------------------------- M9Y P1: deferred finalization
    // Lock order: the worker and the owner take s_QMtx (queue) or s_RetireMtx
    // (retire list), never both nested, and never while holding the wayland
    // lock. FinalizeRecord runs without any of these locks; it is only called
    // from the wlserver event loop context (RetireDrainCallback), the same
    // context the previous inline dispose ran in.
    static void FinalizeRecord( CEglStreamRecord *rec )
    {
        tl4t_es_log( "finalize: slot=%p", (void *)rec->slot );
        if ( rec->slot != nullptr ) { TegraL4TFreeExportedSlot( rec->slot ); rec->slot = nullptr; }
        if ( rec->stream_fd >= 0 ) { close( rec->stream_fd ); rec->stream_fd = -1; }
        wlr_buffer_finish( &rec->base );
        delete rec;
    }

    static void RetireDrainNow()
    {
        std::vector<CEglStreamRecord *> todo;
        {
            std::lock_guard<std::mutex> lk( s_RetireMtx );
            todo.swap( s_RetirePending );
        }
        for ( CEglStreamRecord *rec : todo )
        {
            if ( rec->owner_pins.load() != 0 )
            {
                // A real copy/ack is in flight on the owner side; keep the
                // record pending. The unpin path re-signals the watcher.
                std::lock_guard<std::mutex> lk( s_RetireMtx );
                s_RetirePending.push_back( rec );
                continue;
            }
            FinalizeRecord( rec );
        }
    }

    static int RetireDrainCallback( int fd, uint32_t, void * )
    {
        uint64_t uVal = 0;
        while ( read( fd, &uVal, sizeof( uVal ) ) == (ssize_t)sizeof( uVal ) ) {}
        RetireDrainNow();
        return 0;
    }

    static void RetireSignal()
    {
        if ( s_RetireFd >= 0 )
        {
            uint64_t uOne = 1;
            ssize_t n = write( s_RetireFd, &uOne, sizeof( uOne ) );
            (void)n;
        }
    }

    // RAII pin for the whole owner-side commit sequence (submit -> copy/ack ->
    // release). Finalization waits for pins == 0, so the transport slot cannot
    // be freed between job completion and TegraL4TFetchSlotToTexture.
    struct CRecOwnerPin
    {
        CEglStreamRecord *rec;
        explicit CRecOwnerPin( CEglStreamRecord *r ) : rec( r ) { rec->owner_pins.fetch_add( 1 ); }
        ~CRecOwnerPin() { if ( rec->owner_pins.fetch_sub( 1 ) == 1 ) RetireSignal(); }
    };

    // -------------------------------------------------------------- consumer loop
    static void ConsumerThreadMain()
    {
        if ( !ConsumerEglInit() )
        { tl4t_es_log( "consumer EGL/GL init failed; EGLStream attach will fail closed" ); s_EglOk = false; }
        else s_EglOk = true;

        if ( M9sRetirementEnabled() )
        {
            if ( !s_E.QueryStream || !s_G.FenceSync || !s_G.ClientWaitSync || !s_G.DeleteSync )
                tl4t_es_fail_closed( "stream retirement prerequisites missing" );
            tl4t_es_log( "M9S_RETIRE_PREREQUISITES_OK query=1 fence=1 clientwait=1 deletesync=1 bound_ns=2000000000" );
        }
        for (;;)
        {
            std::shared_ptr<SJob> pJob;
            {
                std::unique_lock<std::mutex> lk( s_QMtx );
                s_QCv.wait( lk, []{ return s_Quit || !s_Queue.empty(); } );
                if ( s_Queue.empty() ) { if ( s_Quit ) break; continue; }
                pJob = s_Queue.front(); s_Queue.pop_front();
            }
            if ( pJob == nullptr ) continue;
            SJob *j = pJob.get();

            bool ok = false;
            switch ( j->kind )
            {
            case SJob::CONNECT:
                ok = s_EglOk && j->rec != nullptr &&
                     GlConnectRecord( j->rec, j->fd, j->width, j->height, j->slot );
                if ( j->fd >= 0 ) { close( j->fd ); j->fd = -1; }
                break;
            case SJob::FRAME:
                ok = s_EglOk && j->rec != nullptr && GlConsumeFrame( j->rec, j->flipY );
                break;
            case SJob::CONSUME:
                ok = s_EglOk && j->rec != nullptr && GlConsumeDiscard( j->rec );
                break;
            case SJob::TEARDOWN:
                if ( j->rec != nullptr ) GlDestroyRecord( j->rec );
                if ( j->detach && j->rec != nullptr )
                {
                    // M9Y P1: consumer-side teardown done; hand the record to the
                    // owner thread for finalization (slot free, wlr_buffer_finish,
                    // delete) after this point. The queue keeps the job alive.
                    CEglStreamRecord *rec = j->rec;
                    {
                        std::lock_guard<std::mutex> lk( s_RetireMtx );
                        s_RetirePending.push_back( rec );
                    }
                    RetireSignal();
                    j->rec = nullptr;
                }
                ok = true;
                break;
            }

            {
                std::lock_guard<std::mutex> lk( j->m );
                j->ok = ok; j->done = true;
            }
            j->cv.notify_all();
        }
    }

    static bool StartConsumer()
    {
        bool expected = false;
        if ( !s_Started.compare_exchange_strong( expected, true ) ) return true;
        s_Thread = std::thread( ConsumerThreadMain );
        return true;
    }

    // ------------------------------------------------------------- wl_buffer glue
    static void EglStreamWlBufferDestroyRequest( ::wl_client *, ::wl_resource *pResource )
    { wl_resource_destroy( pResource ); }

    static const struct wl_buffer_interface s_EglStreamWlBufferImpl = {
        .destroy = EglStreamWlBufferDestroyRequest,
    };

    static void EglStreamSendRelease( CEglStreamRecord *rec )
    {
        wlserver_lock();
        if ( rec->buf_res != nullptr )
            wl_buffer_send_release( rec->buf_res );
        wlserver_unlock();
    }

    // The wl_resource holds ONE wlr_buffer reference; wlroots may also lock it
    // during a commit. Freeing happens only when the last reference drops
    // (EglStreamBufferDestroyImpl), never directly from the resource destroy.
    static void EglStreamBufferResourceDestroy( ::wl_resource *pResource )
    {
        CEglStreamRecord *rec = (CEglStreamRecord *) wl_resource_get_user_data( pResource );
        if ( rec == nullptr ) return;
        rec->buf_res = nullptr;
        rec->destroyed = true;
        wlr_buffer_drop( &rec->base );
    }

    static bool EglStreamResIsInstance( ::wl_resource *pResource )
    { return wl_resource_instance_of( pResource, &wl_buffer_interface, &s_EglStreamWlBufferImpl ); }

    static ::wlr_buffer *EglStreamResFrom( ::wl_resource *pResource )
    {
        CEglStreamRecord *rec = (CEglStreamRecord *) wl_resource_get_user_data( pResource );
        return rec != nullptr ? &rec->base : nullptr;
    }

    static const ::wlr_buffer_resource_interface s_EglStreamResourceIface = {
        .name = "tegra_l4t_eglstream_buffer",
        .is_instance = EglStreamResIsInstance,
        .from_resource = EglStreamResFrom,
    };

    // Called (refcount 0) once the last user released the buffer: the generation
    // has already seen its last copy fence + wl_buffer.release, so tear the
    // consumer side down on the owner thread, free the slot, then free the record.
    static void EglStreamBufferDestroyImpl( ::wlr_buffer *pBuf )
    {
        CEglStreamRecord *rec = reinterpret_cast<CEglStreamRecord *>( pBuf );
        if ( rec == nullptr ) return;

        if ( rec->surface_destroy_listener_added != nullptr )
        { wl_list_remove( &rec->surface_destroy_listener.link );
          rec->surface_destroy_listener_added = nullptr; }

        if ( rec->destroy_started.exchange( true ) )
        { tl4t_es_log( "dispose: duplicate destroy ignored" ); return; }

        if ( rec->retired && rec->connected && !rec->quiescent )
            tl4t_es_fail_closed( "retired stream is not GPU quiescent" );

        rec->retiring.store( true );
        RetireDrainNow();   // opportunistic drain on the owner path

        if ( rec->connected )
        {
            // M9Y P1: consumer teardown runs on the consumer thread; the record
            // (slot, fds, wlr_buffer, memory) is finalized on this loop only
            // after the teardown job is confirmed complete. Nothing is freed
            // because a caller stopped waiting; the transport slot is not freed
            // under an in-flight copy (owner pins gate the drain).
            SubmitJobTeardown( rec );
            tl4t_es_log( "dispose: retired; host finalize after consumer teardown" );
            return;
        }

        // Never connected: no consumer-side objects; finalize on this path.
        FinalizeRecord( rec );
    }

    // ------------------------------------------------------------ surface teardown
    static void EglStreamSurfaceDestroy( ::wl_listener *l, void * )
    {
        CEglStreamRecord *rec = wl_container_of( l, rec, surface_destroy_listener );
        // Safe here: the surface resource (and its destroy signal list) is still
        // alive while its destroy listeners are being emitted.
        wl_list_remove( &rec->surface_destroy_listener.link );
        rec->surface_destroy_listener_added = nullptr;
        rec->surface_res = nullptr;
        // The buffer resource destroy path performs the actual teardown.
    }

    // --------------------------------------------------------------- wl_eglstream
    static void DisplaySendCaps( ::wl_resource *pResource )
    {
        wl_eglstream_display_send_caps( pResource, WL_EGLSTREAM_DISPLAY_CAP_STREAM_FD );
    }

    static void DisplayCreateStream( ::wl_client *pClient, ::wl_resource *pResource, uint32_t uId,
        int32_t nWidth, int32_t nHeight, int32_t nHandle, int32_t nType, struct wl_array *pAttribs )
    {
        const uint32_t kMaxDim = 7680;
        if ( nType != WL_EGLSTREAM_HANDLE_TYPE_FD || nHandle < 0 || nWidth <= 0 || nHeight <= 0 ||
             (uint32_t)nWidth > kMaxDim || (uint32_t)nHeight > kMaxDim )
        {
            if ( nHandle >= 0 ) close( nHandle );
            wl_resource_post_error( pResource, WL_EGLSTREAM_ERROR_BAD_ALLOC, "bad create_stream" );
            return;
        }

        CEglStreamRecord *rec = new CEglStreamRecord();
        rec->stream_fd = nHandle;
        rec->width = (uint32_t)nWidth;
        rec->height = (uint32_t)nHeight;

        if ( pAttribs != nullptr && pAttribs->size >= sizeof(intptr_t) )
        {
            const intptr_t *a = (const intptr_t *) pAttribs->data;
            size_t n = pAttribs->size / sizeof(intptr_t);
            for ( size_t i = 0; i + 1 < n; i += 2 )
                if ( a[i] == WL_EGLSTREAM_ATTRIB_Y_INVERTED ) rec->flipY = a[i+1] ? 1 : 0;
        }

        wlr_buffer_init( &rec->base, &s_EglStreamBufferImpl, nWidth, nHeight );

        ::wl_resource *pBuf = wl_resource_create( pClient, &wl_buffer_interface, 1, uId );
        if ( pBuf == nullptr )
        {
            wl_client_post_no_memory( pClient );
            if ( rec->stream_fd >= 0 ) close( rec->stream_fd );
            wlr_buffer_finish( &rec->base );
            delete rec;
            return;
        }
        rec->buf_res = pBuf;
        wl_resource_set_implementation( pBuf, &s_EglStreamWlBufferImpl, rec, EglStreamBufferResourceDestroy );

        tl4t_es_log( "create_stream: wl_buffer id=%u %dx%d fd=%d flip=%d",
                     uId, nWidth, nHeight, nHandle, rec->flipY );
    }

    static void DisplaySwapInterval( ::wl_client *, ::wl_resource *, ::wl_resource *pStream, int32_t nInterval )
    { tl4t_es_log( "swap_interval(%d) ignored", nInterval ); (void)pStream; }

    static const struct wl_eglstream_display_interface s_DisplayImpl = {
        .create_stream = DisplayCreateStream,
        .swap_interval = DisplaySwapInterval,
    };

    static void DisplayBind( ::wl_client *pClient, void *, uint32_t uVersion, uint32_t uId )
    {
        ::wl_resource *pResource = wl_resource_create( pClient, &wl_eglstream_display_interface,
                                                       uVersion < 1u ? uVersion : 1u, uId );
        if ( pResource == nullptr ) { wl_client_post_no_memory( pClient ); return; }
        wl_resource_set_implementation( pResource, &s_DisplayImpl, nullptr, nullptr );
        DisplaySendCaps( pResource );
    }

    // ---------------------------------------------------- wl_eglstream_controller
    static void ControllerAttach( ::wl_client *, ::wl_resource *, ::wl_resource *pSurface, ::wl_resource *pBuffer )
    {
        CEglStreamRecord *rec = (CEglStreamRecord *) wl_resource_get_user_data( pBuffer );
        if ( rec == nullptr )
        { tl4t_es_log( "attach: buffer is not an EGLStream buffer" ); return; }
        if ( rec->destroyed )
        { tl4t_es_log( "attach: buffer already destroyed" ); return; }
        if ( rec->retired ) return; // never retry this refused generation
        if ( rec->connected )
        { tl4t_es_log( "attach: already connected (idempotent)" ); return; }

        rec->surface_res = pSurface;
        rec->attached = true;

        // §2/§4: build the compositor-produced slot for this generation.
        rec->slot = TegraL4TCreateExportedSlot( rec->width, rec->height );
        TegraL4TSlotImportInfo info{};
        if ( rec->slot == nullptr || !TegraL4TGetSlotImportInfo( rec->slot, &info ) )
        { TegraL4TFreeExportedSlot( rec->slot ); rec->slot = nullptr;
          tl4t_es_fail_closed( "cannot create/export transport slot" ); }

        // §3: the connect MUST have completed before this handler returns.
        int fd = rec->stream_fd; rec->stream_fd = -1;
        auto pConn = std::make_shared<SJob>();
        pConn->kind = SJob::CONNECT; pConn->rec = rec; pConn->fd = fd;
        pConn->width = rec->width; pConn->height = rec->height; pConn->flipY = rec->flipY; pConn->slot = info;
        ESubmitResult eConn = SubmitJobSync( pConn, 5000 );
        if ( eConn == SUBMIT_REFUSED )
        { if ( fd >= 0 ) close( fd );
          tl4t_es_log( "attach: refused (generation retiring)" );
          return; }
        if ( eConn == SUBMITTED_TIMEOUT )
        {   // the job owns the fd now; fail closed without racing its close
            tl4t_es_fail_closed( "consumer connect timed out" ); }
        if ( !pConn->ok )
        {
            // SubmitJobSync succeeded: worker completed and closed its fd.
            // Failure occurred before GL allocation/import/submission. Thus the
            // unimported transport slot can be freed; no late GPU work exists.
            if ( M9sRetirementEnabled() && M9sCanRejectUnimportable( rec->width, rec->height,
                     rec->connect_error, rec->stream, rec->connected ) )
            {
                rec->retired = true; rec->quiescent = true;
                TegraL4TFreeExportedSlot( rec->slot ); rec->slot = nullptr;
                rec->surface_res = nullptr;
                tl4t_es_log( "M9S_BUFFER_REJECTED buffer=%u surface=%u error=0x%04x slot_freed=1 retry=0",
                    wl_resource_get_id(pBuffer), pSurface ? wl_resource_get_id(pSurface) : 0,
                    (unsigned)rec->connect_error );
                // Existing !connected commit path sends wl_buffer.release and
                // returns nullptr; no texture or frame success is fabricated.
                return;
            }
            tl4t_es_fail_closed( "eglStreamConsumerGLTextureExternalKHR connect failed" );
        }

        rec->connected = true;
        if ( pSurface != nullptr && rec->surface_destroy_listener_added != pSurface )
        {
            rec->surface_destroy_listener.notify = EglStreamSurfaceDestroy;
            wl_resource_add_destroy_listener( pSurface, &rec->surface_destroy_listener );
            rec->surface_destroy_listener_added = pSurface;
        }

        tl4t_es_log( "attach: connected stream %ux%d (per-generation slot ready)", rec->width, rec->height );
    }

    static void ControllerAttachAttribs( ::wl_client *c, ::wl_resource *r, ::wl_resource *pSurface,
                                         ::wl_resource *pBuffer, struct wl_array *pAttribs )
    { (void)pAttribs; ControllerAttach( c, r, pSurface, pBuffer ); }

    static const struct wl_eglstream_controller_interface s_ControllerImpl = {
        .attach_eglstream_consumer = ControllerAttach,
        .attach_eglstream_consumer_attribs = ControllerAttachAttribs,
    };

    static void ControllerBind( ::wl_client *pClient, void *, uint32_t uVersion, uint32_t uId )
    {
        ::wl_resource *pResource = wl_resource_create( pClient, &wl_eglstream_controller_interface,
                                                       uVersion < 2u ? uVersion : 2u, uId );
        if ( pResource == nullptr ) { wl_client_post_no_memory( pClient ); return; }
        wl_resource_set_implementation( pResource, &s_ControllerImpl, nullptr, nullptr );
    }

    // ------------------------------------------------------------------ public API
    bool TegraL4TInitEglStream( ::wl_display *pDisplay )
    {
        static bool s_bInited = false;
        if ( s_bInited ) return true;
        s_bInited = true;

        wlr_buffer_register_resource_interface( &s_EglStreamResourceIface );
        wl_global_create( pDisplay, &wl_eglstream_display_interface, 1, nullptr, DisplayBind );
        wl_global_create( pDisplay, &wl_eglstream_controller_interface, 2, nullptr, ControllerBind );
        StartConsumer();

        // M9Y P1: bounded owner-side finalization watcher. The eventfd is
        // drained on the wlserver event loop (the same context the previous
        // inline dispose ran in); the consumer thread never frees a record.
        s_RetireFd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
        if ( s_RetireFd >= 0 )
        {
            struct wl_event_loop *pLoop = wl_display_get_event_loop( pDisplay );
            s_RetireSource = wl_event_loop_add_fd( pLoop, s_RetireFd, WL_EVENT_READABLE, RetireDrainCallback, nullptr );
            if ( s_RetireSource == nullptr )
            { close( s_RetireFd ); s_RetireFd = -1;
              tl4t_es_fail_closed( "retire watcher registration failed" ); }
            tl4t_es_log( "retire watcher registered; host finalize on the wlserver loop" );
        }
        else
            tl4t_es_fail_closed( "retire eventfd failed" );

        tl4t_es_log( "EGLStream consumer globals registered (display v1 + controller v2)" );
        return true;
    }

    bool TegraL4TIsEglStreamBuffer( ::wlr_buffer *pBuffer )
    {
        return pBuffer != nullptr && pBuffer->impl == &s_EglStreamBufferImpl;
    }

    OwningRc<CVulkanTexture> TegraL4TCreateTextureFromEglStreamBufferWithYFlip( ::wlr_buffer *pBuffer, bool bExtraYFlip )
    {
        if ( !TegraL4TIsEglStreamBuffer( pBuffer ) )
            return nullptr;
        CEglStreamRecord *rec = reinterpret_cast<CEglStreamRecord *>( pBuffer );
        if ( rec->destroyed || rec->retired || !rec->connected || rec->slot == nullptr )
        {
            tl4t_es_log( "commit: stream not connected; releasing buffer" );
            EglStreamSendRelease( rec );
            return nullptr;
        }

        rec->commit_serial++;

        // M9Y P1: owner pin covers the whole owner-side sequence (submit ->
        // copy/ack -> release); host finalization cannot free the slot meanwhile.
        CRecOwnerPin ownerPin( rec );

        auto pFrame = std::make_shared<SJob>();
        pFrame->kind = SJob::FRAME; pFrame->rec = rec;
        pFrame->flipY = (rec->flipY != 0) ^ bExtraYFlip;
        if ( bExtraYFlip && rec->commit_serial <= 3 )
            tl4t_es_log( "m9n-game-orientation: extra_yflip=%d protocol_yflip=%d effective_yflip=%d",
                         bExtraYFlip ? 1 : 0, rec->flipY ? 1 : 0, pFrame->flipY );
        ESubmitResult eRes = SubmitJobSync( pFrame, s_AcquireTimeoutMs + 3000 );
        if ( eRes == SUBMIT_REFUSED )
        { tl4t_es_log( "commit: refused (generation retiring); releasing buffer" );
          EglStreamSendRelease( rec ); return nullptr; }
        if ( eRes == SUBMITTED_TIMEOUT )
        { tl4t_es_log( "commit: consumer frame job timed out; fail closed for this frame" );
          EglStreamSendRelease( rec ); return nullptr; }
        if ( !pFrame->ok )
        { tl4t_es_log( "commit: acquire/blit failed; releasing buffer" );
          EglStreamSendRelease( rec ); return nullptr; }

        // Accepted buffer->image body, unchanged, on this (compositor) thread.
        OwningRc<CVulkanTexture> pTex = TegraL4TFetchSlotToTexture( rec->slot );

        // GPU copy complete and wl_buffer.release may now be sent (Wayland lifetime
        // only; never while holding a GPU wait).
        EglStreamSendRelease( rec );

        if ( rec->commit_serial <= 3 || ( rec->commit_serial % 4 ) == 0 )
            tl4t_es_log( "commit %llu: %dx%d -> %s",
                         (unsigned long long)rec->commit_serial, rec->width, rec->height,
                         pTex ? "texture" : "discard" );
        return pTex;
    }


    OwningRc<CVulkanTexture> TegraL4TCreateTextureFromEglStreamBuffer( ::wlr_buffer *pBuffer )
    {
        return TegraL4TCreateTextureFromEglStreamBufferWithYFlip( pBuffer, false );
    }
    void TegraL4TDiscardEglStreamCommit( ::wlr_buffer *pBuffer, const char *pReason )
    {
        if ( !TegraL4TIsEglStreamBuffer( pBuffer ) )
            return;
        CEglStreamRecord *rec = reinterpret_cast<CEglStreamRecord *>( pBuffer );
        if ( rec->destroyed )
            return;

        // M9Y P1: owner pin covers the whole discard sequence (consume job +
        // release), so host finalization cannot free the slot while it runs.
        CRecOwnerPin ownerPin( rec );

        if ( !rec->retired && rec->connected && rec->slot != nullptr )
        {
            // Consume + release the EGLStream frame (never a silent drop).
            auto pDiscard = std::make_shared<SJob>();
            pDiscard->kind = SJob::CONSUME; pDiscard->rec = rec;
            ESubmitResult eRes = SubmitJobSync( pDiscard, s_AcquireTimeoutMs + 1500 );
            if ( eRes == SUBMIT_REFUSED )
                tl4t_es_log( "discard refused (generation retiring)" );
            else if ( eRes != SUBMITTED_DONE || !pDiscard->ok )
                tl4t_es_log( "discard consume failed/timeout; releasing buffer" );
        }
        EglStreamSendRelease( rec );
        tl4t_es_log( "discard commit (%s)", pReason ? pReason : "?" );
    }
}

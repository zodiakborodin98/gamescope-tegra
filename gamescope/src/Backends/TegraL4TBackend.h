#pragma once

#include "rc.h"

class CVulkanTexture;
struct wlr_buffer;
struct wl_display;

namespace gamescope
{
    // ---------------------------------------------------------------------------
    // M7b gate1 - opt-in Tegra L4T renderer bootstrap.
    //
    // These predicates are consulted from the shared renderer code
    // (rendervulkan.cpp) so that every Tegra-specific decision is taken while the
    // backend is already installed, i.e. before/while the VkDevice is created.
    // They are false unless CTegraL4TBackend was actually selected, so every
    // other backend keeps its existing behaviour.
    // ---------------------------------------------------------------------------

    // True when the Tegra L4T backend is the active backend for this process.
    bool IsTegraL4TMode();

    // M7f: true when the backend is running the *normal* Gamescope session
    // (wlserver + steamcompmgr + real clients) rather than one of the bounded
    // diagnostic opt-ins. Only then is the persistent presenter + client
    // transport active.
    bool TegraL4TIsNormalSessionMode();

    // Records whether the explicit bounded bootstrap diagnostic was requested.
    void TegraL4TSetDiagnosticRequested( bool bRequested );
    bool TegraL4TIsDiagnosticRequested();

    // M7c: records whether the bounded first-frame (composite->GOB->FLIP4) mode
    // was requested.
    void TegraL4TSetFirstFrameRequested( bool bRequested );
    bool TegraL4TIsFirstFrameRequested();

    // M7f2c: records whether the native-Wayland test mode without Xwayland was
    // requested (Tegra L4T only). In that mode no Xwayland server is created,
    // wlserver does not wait for Xwayland readiness, and steamcompmgr omits its
    // X11-only state instead of dereferencing an absent Xwayland context.
    void TegraL4TSetNoXwaylandRequested( bool bRequested );
    bool TegraL4TIsNoXwaylandRequested();

    // Runs the bounded gate1 bootstrap diagnostic against the initialised renderer.
    // Returns true only if the whole bootstrap completed. Never claims presentation.
    bool TegraL4TRunBootstrapDiagnostic();

    // M7c: runs the bounded first-frame path against the initialised renderer:
    // real vulkan_composite -> GOB conversion -> FLIP4 -> own-fence retirement.
    // Returns true only if a frame was composited, converted, presented and its
    // own post fence retired. Never a fake success stub.
    bool TegraL4TRunFirstFrameMode();

    // M7f: register the private client-buffer transport global on the given
    // wl_display and the wlroots buffer-resource interface. Idempotent.
    void TegraL4TInitTransport( ::wl_display *pDisplay );

    // M7f: true if the buffer is one of our private transport buffers.
    bool TegraL4TIsTransportBuffer( ::wlr_buffer *pBuffer );

    // M7f: import such a buffer into a Gamescope-owned RGBA8 VkImage on the
    // compositor's VkDevice/queue (OPAQUE_FD import + GPU buffer->image copy).
    // Returns nullptr on any unsupported/failed metadata check (fail closed).
    gamescope::OwningRc<CVulkanTexture> TegraL4TCreateTextureFromTransportBuffer( ::wlr_buffer *pBuffer );

    // M7f2j: final-discard disposition for a received transport commit that will
    // not be displayed (no window / not the current surface / a failed copy).
    // Completes the commit's ready/returned handshake with the copy path's
    // ownership dance (no copy) so the producer's next slot reuse never blocks.
    // Bounded and fail-closed; must not be called while holding the wlserver
    // lock. No-op for non-transport buffers.
    void TegraL4TDiscardTransportCommit( ::wlr_buffer *pBuffer, const char *pReason );

    // ---------------------------------------------------------------------------
    // M7g - EGLStream consumer: compositor-produced exported transport slot.
    //
    // The dedicated consumer thread (external OES texture / capture FBO / GL
    // imports) writes each acquired frame into a GL storage buffer that wraps
    // THIS slot's OPAQUE_FD memory and signals `ready`; the compositor copies the
    // slot into a fresh Gamescope-owned RGBA8 VkImage on its own queue (via the
    // accepted buffer->image body) and signals `returned` (decisions 4/5).
    // ---------------------------------------------------------------------------
    struct CTegraL4TExportedSlot;

    // Exact metadata the GL side needs to import the slot (memory + semaphores).
    struct TegraL4TSlotImportInfo
    {
        int      mem_fd = -1;
        int      ready_fd = -1;      // GL signals, compositor waits
        int      returned_fd = -1;   // compositor signals, GL waits
        uint32_t allocation_size = 0;
        uint32_t memory_type_index = 0;
        uint32_t memory_type_bits = 0;
        uint32_t memory_property_flags = 0;
        uint32_t dedicated = 0;
        uint8_t  device_uuid[16] = {};
        uint8_t  driver_uuid[16] = {};
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Create + export one per-generation transport slot sized w*h*4 (OPAQUE_FD
    // memory + ready/returned semaphores). Lazy; honours the kMaxDim/kMaxAlloc
    // registry guards. Returns nullptr on any failure. Does NOT generalise the
    // received-transport object CTegraL4TBuffer.
    CTegraL4TExportedSlot *TegraL4TCreateExportedSlot( uint32_t uWidth, uint32_t uHeight );
    bool TegraL4TGetSlotImportInfo( CTegraL4TExportedSlot *pSlot, TegraL4TSlotImportInfo *pOut );
    // Wait ready -> copy slot -> fresh image (accepted body) -> signal returned.
    gamescope::OwningRc<CVulkanTexture> TegraL4TFetchSlotToTexture( CTegraL4TExportedSlot *pSlot );
    // No-copy acknowledgment: wait ready -> acquire/release -> signal returned.
    void TegraL4TAckSlotDiscard( CTegraL4TExportedSlot *pSlot );
    void TegraL4TFreeExportedSlot( CTegraL4TExportedSlot *pSlot );

    // ---------------------------------------------------------------------------
    // M7g - EGLStream consumer (wl_eglstream_display v1 + wl_eglstream_controller
    // v2). Globals are registered when IsTegraL4TMode(). The consumer owns its own
    // thread + EGLDisplay/context; attach blocks (bounded) until the consumer
    // exists so Xwayland's sync `done` cannot be flushed early (decision 3).
    // ---------------------------------------------------------------------------
    bool TegraL4TInitEglStream( ::wl_display *pDisplay );
    bool TegraL4TIsEglStreamBuffer( ::wlr_buffer *pBuffer );
    gamescope::OwningRc<CVulkanTexture> TegraL4TCreateTextureFromEglStreamBuffer( ::wlr_buffer *pBuffer );
    void TegraL4TDiscardEglStreamCommit( ::wlr_buffer *pBuffer, const char *pReason );

    // M7f: stop the persistent presenter (DC close while owning, head restore)
    // before the renderer VkDevice is torn down. Returns false when the DC
    // close/restore could not be confirmed (caller must exit nonzero without
    // running destructors). No-op true in diagnostic modes.
    bool TegraL4TPresenterShutdown();

    // M7d: release the process-lifetime owning renderer resources (typed
    // bootstrap dummies plus the deferred output/NIS/blank images) while the
    // VkDevice and NVIDIA driver are still alive, so the static-exit destructors
    // do not call into a torn-down driver. No-op unless the Tegra L4T backend is
    // active, so every other backend is unaffected. Must be called after the
    // bounded renderer wait and after the frame function's DC/GOB cleanup.
    void TegraL4TReleaseOwnedResources();
}

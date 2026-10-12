#include "backend.h"
#include "Backends/TegraL4TBackend.h"
#include "Backends/tegra_l4t/tegra_l4t_first_frame.h"
#include "Backends/tegra_l4t/tegra_l4t_presenter.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"

#include <cstdio>
#include <cstring>
#include <cinttypes>

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    // ---------------------------------------------------------------------------
    // M7b gate1 state. Set only when CTegraL4TBackend is actually constructed, so
    // shared renderer code can branch on it without affecting any other backend.
    // ---------------------------------------------------------------------------
    static bool s_bTegraL4TMode = false;
    static bool s_bDiagnosticRequested = false;
    static bool s_bFirstFrameRequested = false;
    // M7f2c: native-Wayland test mode without Xwayland (no server, no wait).
    static bool s_bNoXwaylandRequested = false;
    // M7f: normal session (wlserver + steamcompmgr + real clients).
    static bool s_bNormalSession = false;
    // M7f: the single live presenter, for deterministic shutdown from main().
    static CTegraL4TPresenter *s_pActivePresenter = nullptr;

    bool IsTegraL4TMode()
    {
        return s_bTegraL4TMode;
    }

    bool TegraL4TIsNormalSessionMode()
    {
        return s_bNormalSession;
    }

    void TegraL4TSetDiagnosticRequested( bool bRequested )
    {
        s_bDiagnosticRequested = bRequested;
    }

    bool TegraL4TIsDiagnosticRequested()
    {
        return s_bDiagnosticRequested;
    }

    void TegraL4TSetFirstFrameRequested( bool bRequested )
    {
        s_bFirstFrameRequested = bRequested;
    }

    bool TegraL4TIsFirstFrameRequested()
    {
        return s_bFirstFrameRequested;
    }

    void TegraL4TSetNoXwaylandRequested( bool bRequested )
    {
        s_bNoXwaylandRequested = bRequested;
    }

    bool TegraL4TIsNoXwaylandRequested()
    {
        return s_bNoXwaylandRequested;
    }

    class CTegraL4TConnector final : public CBaseBackendConnector
    {
    public:
        CTegraL4TConnector() {}
        virtual ~CTegraL4TConnector() {}

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override { return false; }
        virtual bool IsHDRActive() const override { return false; }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override { return m_HDRInfo; }
        virtual bool IsVRRActive() const override { return false; }
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }
        virtual bool SupportsVRR() const override { return false; }
        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{};
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
            *displayColorimetry = displaycolorimetry_709;
            *displayEOTF = EOTF_Gamma22;
            *outputEncodingColorimetry = displaycolorimetry_709;
            *outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override { return "Tegra L4T"; }
        virtual const char *GetMake() const override { return "NVIDIA"; }
        virtual const char *GetModel() const override { return "Tegra X1 built-in LCD (not presented in gate1)"; }

        virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
        {
            if ( !s_bNormalSession )
            {
                // Diagnostic modes never reach Present().
                return 0;
            }

            if ( !m_Presenter.Ready() )
            {
                if ( !m_Presenter.Init( g_nOutputWidth != 0 ? g_nOutputWidth : 1280,
                                        g_nOutputHeight != 0 ? g_nOutputHeight : 720,
                                        /*orientation*/ 90,
                                        /*block_height_log2*/ 4,
                                        /*dc*/ -1, /*window*/ 0,
                                        g_nOutputRefresh ) )
                {
                    fprintf( stderr, "[tegra-l4t] persistent presenter init FAILED; not presenting this frame\n" );
                    return -1;
                }
                s_pActivePresenter = &m_Presenter;
            }

            return m_Presenter.PresentFrame( pFrameInfo );
        }

    private:
        BackendConnectorHDRInfo m_HDRInfo{};
        CTegraL4TPresenter m_Presenter;
    };

    class CTegraL4TBackend final : public CBaseBackend
    {
    public:
        CTegraL4TBackend()
        {
            s_bTegraL4TMode = true;
        }

        virtual ~CTegraL4TBackend() {}

        virtual bool Init() override
        {
            s_bNormalSession = !s_bDiagnosticRequested && !s_bFirstFrameRequested;

            g_nOutputWidth = g_nPreferredOutputWidth;
            g_nOutputHeight = g_nPreferredOutputHeight;
            g_nOutputRefresh = g_nNestedRefresh;

            if ( g_nOutputHeight == 0 )
            {
                if ( g_nOutputWidth != 0 )
                {
                    fprintf( stderr, "Cannot specify -W without -H\n" );
                    return false;
                }
                g_nOutputHeight = 720;
            }
            if ( g_nOutputWidth == 0 )
                g_nOutputWidth = g_nOutputHeight * 16 / 9;
            if ( g_nOutputRefresh == 0 )
                g_nOutputRefresh = ConvertHztomHz( 60 );

            fprintf( stderr, "[tegra-l4t] bootstrap: backend selected; logical output %dx%d @ %umHz\n",
                     g_nOutputWidth, g_nOutputHeight, g_nOutputRefresh );

            // Initialise the real Gamescope Vulkan renderer (instance already built
            // with this backend's instance extensions). No wlsession_init(), no host
            // DISPLAY/WAYLAND_DISPLAY, no /dev/dri, no Xwayland/wlserver, no Steam.
            if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
            {
                fprintf( stderr, "[tegra-l4t] bootstrap: vulkan_init failed\n" );
                return false;
            }

            fprintf( stderr, "[tegra-l4t] bootstrap: renderer initialised\n" );

            if ( s_bNormalSession )
            {
                // M7f: start the normal wlserver session (non-session backend:
                // no DRM/seat). Creates the wl_display + headless wlroots backend
                // that wlserver_init()/steamcompmgr then drive. Presentation is
                // owned by the persistent Tegra presenter in Present().
                if ( !wlsession_init() )
                {
                    fprintf( stderr, "[tegra-l4t] wlsession_init failed\n" );
                    return false;
                }
                fprintf( stderr, "[tegra-l4t] normal session mode: wlsession initialised (non-session backend)\n" );
            }

            return true;
        }

        virtual bool PostInit() override
        {
            if ( s_bNormalSession )
            {
                fprintf( stderr, "[tegra-l4t] PostInit: normal session accepted\n" );
                return true;
            }
            fprintf( stderr, "[tegra-l4t] PostInit refused (session/steamcompmgr path not implemented in gate1)\n" );
            return false;
        }

        virtual std::span<const char *const> GetInstanceExtensions() const override
        {
            return std::span<const char *const>{};
        }
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            return std::span<const char *const>{};
        }
        virtual VkImageLayout GetPresentLayout() const override
        {
            return VK_IMAGE_LAYOUT_GENERAL;
        }
        virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
        {
            // RGB 32-bit only (R8G8B8A8_UNORM). No modifiers, no YCbCr.
            *pPrimaryPlaneFormat = VulkanFormatToDRM( VK_FORMAT_R8G8B8A8_UNORM );
            *pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_R8G8B8A8_UNORM );
        }
        virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
        {
            return true;
        }

        virtual void DirtyState( bool bForce, bool bForceModeset ) override {}
        virtual bool PollState() override { return false; }

        virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
        {
            return std::make_shared<BackendBlob>( data );
        }

        virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
        {
            // Explicit failure: no client DMA-BUF import in Tegra mode. gate1 does
            // not claim any client buffer compatibility.
            fprintf( stderr, "[tegra-l4t] ImportDmabufToBackend: refused (no client DMA-BUF import)\n" );
            return nullptr;
        }

        virtual bool UsesModifiers() const override { return false; }
        virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
        {
            return std::span<const uint64_t>{};
        }

        virtual IBackendConnector *GetCurrentConnector() override { return &m_Connector; }
        virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
        {
            if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
                return &m_Connector;
            return nullptr;
        }

        virtual bool SupportsPlaneHardwareCursor() const override { return false; }
        virtual bool SupportsTearing() const override { return false; }
        virtual bool UsesVulkanSwapchain() const override { return false; }

        virtual bool IsSessionBased() const override { return false; }
        virtual bool SupportsExplicitSync() const override { return false; }

        virtual bool IsPaused() const override { return false; }
        virtual bool IsVisible() const override { return true; }

        virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override { return uvecSize; }
        virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override { return false; }
        virtual void HackUpdatePatchedEdid() override {}

    protected:
        virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override {}

    private:
        CTegraL4TConnector m_Connector;
    };

    /////////////////////////
    // Backend Instantiator
    /////////////////////////

    template <>
    bool IBackend::Set<CTegraL4TBackend>()
    {
        return Set( new CTegraL4TBackend{} );
    }

    // ---------------------------------------------------------------------------
    // Bounded gate1 bootstrap diagnostic. Read-only wrt the renderer: it inspects
    // caps and the resources that the bootstrap already created. No presentation.
    // ---------------------------------------------------------------------------
    bool TegraL4TRunBootstrapDiagnostic()
    {
        if ( !IsTegraL4TMode() )
            return false;

        fprintf( stderr, "[tegra-l4t] ==== bootstrap diagnostic begin ====\n" );

        VkPhysicalDeviceProperties props{};
        g_device.vk.GetPhysicalDeviceProperties( g_device.physDev(), &props );
        fprintf( stderr, "[tegra-l4t] device: %s vendorID=0x%04x deviceID=0x%04x apiVersion=%u.%u.%u driverVersion=0x%08x\n",
                 props.deviceName, props.vendorID, props.deviceID,
                 VK_VERSION_MAJOR( props.apiVersion ), VK_VERSION_MINOR( props.apiVersion ), VK_VERSION_PATCH( props.apiVersion ),
                 props.driverVersion );

        const std::vector<VkExtensionProperties> &exts = g_device.supportedExtensions();
        auto HasExt = [&exts]( const char *pName ) -> bool
        {
            for ( const auto &e : exts )
                if ( strcmp( e.extensionName, pName ) == 0 )
                    return true;
            return false;
        };
        fprintf( stderr, "[tegra-l4t] ext present: external_memory_fd=%d external_semaphore_fd=%d dma_buf=%d robustness2=%d physical_device_drm=%d\n",
                 HasExt( "VK_KHR_external_memory_fd" ), HasExt( "VK_KHR_external_semaphore_fd" ),
                 HasExt( "VK_EXT_external_memory_dma_buf" ), HasExt( "VK_EXT_robustness2" ),
                 HasExt( "VK_EXT_physical_device_drm" ) );
        fprintf( stderr, "[tegra-l4t] rgb-only profile: ycbcr conversion omitted, binding4 ordinary immutable samplers; ycbcr masks forced to 0\n" );

        VkFormatProperties fmtProps{};
        g_device.vk.GetPhysicalDeviceFormatProperties( g_device.physDev(), VK_FORMAT_R8G8B8A8_UNORM, &fmtProps );
        const bool bRgba8Storage = ( fmtProps.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ) != 0;
        const bool bRgba8Sampled = ( fmtProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) != 0;
        fprintf( stderr, "[tegra-l4t] caps R8G8B8A8_UNORM: storage_image=%d sampled_image=%d (caps only, not exercised)\n",
                 bRgba8Storage ? 1 : 0, bRgba8Sampled ? 1 : 0 );

        // M7b2: init_nis_data() returns true even when an image allocation fails, so
        // the diagnostic itself must validate every required resource before claiming
        // success. All of these must be present for rc=0; any missing one -> false
        // (main then exits non-zero). This is success validation only.
        const bool bTypedResources = g_device.tegraBootstrapResourcesReady();
        const bool bNisScaler = ( g_output.nisScalerImage != nullptr );
        const bool bNisUsm = ( g_output.nisUsmImage != nullptr );
        const bool bBlankImage = ( g_output.temporaryHackyBlankImage != nullptr );

        fprintf( stderr, "[tegra-l4t] bootstrap resources: typed 1D/2D/3D=%d nisScaler=%d nisUsm=%d blank=%d (all non-exportable)\n",
                 bTypedResources ? 1 : 0, bNisScaler ? 1 : 0, bNisUsm ? 1 : 0, bBlankImage ? 1 : 0 );

        fprintf( stderr, "[tegra-l4t] descriptor dummy-view substitution: UNPROVEN (descriptors not exercised until gate2)\n" );
        fprintf( stderr, "[tegra-l4t] presentation/first-frame: not attempted\n" );

        if ( !bTypedResources || !bNisScaler || !bNisUsm || !bBlankImage || !bRgba8Storage || !bRgba8Sampled )
        {
            fprintf( stderr, "[tegra-l4t] ==== bootstrap diagnostic INCOMPLETE (rc=1): missing required resource/cap ====\n" );
            return false;
        }
        fprintf( stderr, "[tegra-l4t] ==== bootstrap diagnostic complete (rc=0) ====\n" );
        return true;
    }

    // ---------------------------------------------------------------------------
    // M7c bounded first-frame mode: real composite -> GOB -> FLIP4 -> retirement.
    // ---------------------------------------------------------------------------
    bool TegraL4TRunFirstFrameMode()
    {
        if ( !IsTegraL4TMode() )
            return false;

        TegraFirstFrameConfig cfg{};
        cfg.logical_w = g_nOutputWidth != 0 ? g_nOutputWidth : 1280;
        cfg.logical_h = g_nOutputHeight != 0 ? g_nOutputHeight : 720;
        cfg.orientation = 90;        // GOB owns the 90 deg rotation (matches M3e)
        cfg.block_height_log2 = 4;
        cfg.frame_count = 3;         // bounded
        cfg.refresh_mhz = g_nOutputRefresh;
        cfg.window = 0;
        cfg.dc = -1;                 // autodetect head

        return TegraL4TRunFirstFrame( cfg );
    }

    bool TegraL4TPresenterShutdown()
    {
        if ( s_pActivePresenter != nullptr )
        {
            bool bOk = s_pActivePresenter->Shutdown();
            s_pActivePresenter = nullptr;
            return bOk;
        }
        return true;
    }

    void TegraL4TReleaseOwnedResources()
    {
        if ( !IsTegraL4TMode() )
            return;

        // Explicitly free the process-lifetime owning textures while the device
        // and driver are alive (finite wait + command-buffer/dummy release first).
        vulkan_release_owned_resources();
    }
}

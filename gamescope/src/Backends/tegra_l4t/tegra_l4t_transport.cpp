// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7f2 - private Tegra L4T GPU client buffer transport (server side).
//
// Per-slot IMMUTABLE registration of a cross-process OPAQUE_FD Vulkan buffer
// plus its ready/returned binary semaphores and exact allocation metadata. The
// registration object is a real wl_buffer (attached to a real wl_surface and
// committed), so Gamescope/wlroots own surface/commit/lifetime.
//
// The CPU only handles metadata + fds. Each *committed generation* is copied on
// the compositor's own VkDevice/queue into a fresh Gamescope-owned RGBA8 VkImage:
//     wait ready -> ACQUIRE EXTERNAL->consumer -> copy buffer->image ->
//     image TRANSFER_WRITE->SHADER_READ barrier -> RELEASE buffer->EXTERNAL ->
//     signal returned; only after the copy fence is complete is wl_buffer.release
//     sent (under the wlserver lock, never while holding a GPU wait).
//
// The allocation/import semantics mirror the reviewed M6b2 OPAQUE_FD probe
// (preserve memoryTypeIndex/allocationSize/dedicated, validate local req/caps).

#include "Backends/TegraL4TBackend.h"

#include "rendervulkan.hpp"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "wlr_begin.hpp"
#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include "wlr_end.hpp"

#include "wlserver.hpp"

#include "tegra-l4t-buffer-protocol.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cerrno>
#include <unistd.h>
#include <dlfcn.h>

#ifndef VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT
#define VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT 0x00000001
#endif

namespace gamescope
{
    static void tl4t_log( const char *pFmt, ... )
    {
        va_list args; va_start( args, pFmt );
        fprintf( stderr, "[tegra-l4t-xport] " );
        vfprintf( stderr, pFmt, args ); fprintf( stderr, "\n" );
        fflush( stderr );
        va_end( args );
    }

    // ---------------------------------------------------------------- device fns
    static PFN_vkGetBufferMemoryRequirements2 s_pfnGetBufferMemReq2 = nullptr;
    static PFN_vkGetPhysicalDeviceExternalBufferProperties s_pfnGetExtBufProps = nullptr;

    static void tl4t_resolve_fns()
    {
        if ( s_pfnGetBufferMemReq2 == nullptr )
            s_pfnGetBufferMemReq2 = (PFN_vkGetBufferMemoryRequirements2)
                g_device.vk.GetDeviceProcAddr( g_device.device(), "vkGetBufferMemoryRequirements2" );
        if ( s_pfnGetExtBufProps == nullptr )
            s_pfnGetExtBufProps = (PFN_vkGetPhysicalDeviceExternalBufferProperties)
                dlsym( RTLD_DEFAULT, "vkGetPhysicalDeviceExternalBufferProperties" );
    }

    // ---------------------------------------------------------------- buffer
    struct CTegraL4TBuffer
    {
        ::wlr_buffer base;

        int      mem_fd = -1;              // kept only for close-on-destroy bookkeeping
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t drm_format = 0;
        uint32_t buffer_size = 0;
        uint32_t buffer_usage = 0;
        uint32_t buffer_sharing = 0;
        uint32_t allocation_size = 0;
        uint32_t memory_type_index = 0;
        uint32_t memory_type_bits = 0;
        uint32_t memory_property_flags = 0;
        uint32_t dedicated = 0;
        uint8_t  device_uuid[16] = {};
        uint8_t  driver_uuid[16] = {};

        VkBuffer       vkBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vkMemory = VK_NULL_HANDLE;
        VkSemaphore    ready = VK_NULL_HANDLE;     // producer signals, consumer waits
        VkSemaphore    returned = VK_NULL_HANDLE;  // consumer signals, producer waits

        ::wl_resource *resource = nullptr;
        // M7f2j: per-buffer counter of RECEIVED commit events (one increment per
        // disposition round: copy or discard), i.e. the "commit generation" used
        // by the disposition diagnostics - not a count of successful copies.
        uint64_t commit_serial = 0;

        bool imported = false;                      // registration succeeded
        bool release_pending = false;
    };

    // M7g - compositor-produced exported transport slot (see TegraL4TBackend.h and
    // the seam at the end of this TU). Distinct from CTegraL4TBuffer: not a
    // received client buffer, carries its own export fds for the GL consumer.
    struct CTegraL4TExportedSlot
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t allocation_size = 0;
        uint32_t memory_type_index = 0;
        uint32_t memory_type_bits = 0;
        uint32_t memory_property_flags = 0;
        uint32_t dedicated = 0;
        uint8_t  device_uuid[16] = {};
        uint8_t  driver_uuid[16] = {};

        VkBuffer       vkBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vkMemory = VK_NULL_HANDLE;
        VkSemaphore    ready = VK_NULL_HANDLE;     // GL (producer) signals, compositor waits
        VkSemaphore    returned = VK_NULL_HANDLE;  // compositor signals, GL waits
        int            mem_fd = -1;
        int            ready_fd = -1;
        int            returned_fd = -1;
    };

    static void tl4t_buffer_destroy( ::wlr_buffer *pBuf )
    {
        CTegraL4TBuffer *p = reinterpret_cast<CTegraL4TBuffer *>( pBuf );
        VkDevice dev = g_device.device();
        // Our imported handles are ours to release. Never free the producer's
        // memory object: the kernel keeps it alive while the peer holds a ref.
        if ( p->ready ) g_device.vk.DestroySemaphore( dev, p->ready, nullptr );
        if ( p->returned ) g_device.vk.DestroySemaphore( dev, p->returned, nullptr );
        if ( p->vkBuffer ) g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr );
        if ( p->vkMemory ) g_device.vk.FreeMemory( dev, p->vkMemory, nullptr );
        if ( p->mem_fd >= 0 ) { close( p->mem_fd ); p->mem_fd = -1; }
        wlr_buffer_finish( &p->base );
        delete p;
    }

    static const ::wlr_buffer_impl s_TransportBufferImpl = { .destroy = tl4t_buffer_destroy };

    bool TegraL4TIsTransportBuffer( ::wlr_buffer *pBuffer )
    {
        return pBuffer != nullptr && pBuffer->impl == &s_TransportBufferImpl;
    }

    // ---------------------------------------------------------------- wl_buffer
    static void tl4t_wl_buffer_destroy_request( ::wl_client *, ::wl_resource *pResource )
    { wl_resource_destroy( pResource ); }

    static const struct wl_buffer_interface s_TransportWlBufferImpl = {
        .destroy = tl4t_wl_buffer_destroy_request,
    };

    static void tl4t_wl_buffer_resource_destroy( ::wl_resource *pResource )
    {
        CTegraL4TBuffer *p = (CTegraL4TBuffer *) wl_resource_get_user_data( pResource );
        if ( p != nullptr ) { p->resource = nullptr; wlr_buffer_drop( &p->base ); }
    }

    static bool tl4t_res_is_instance( ::wl_resource *pResource )
    { return wl_resource_instance_of( pResource, &wl_buffer_interface, &s_TransportWlBufferImpl ); }

    static ::wlr_buffer *tl4t_res_from( ::wl_resource *pResource )
    {
        CTegraL4TBuffer *p = (CTegraL4TBuffer *) wl_resource_get_user_data( pResource );
        return p != nullptr ? &p->base : nullptr;
    }

    static const ::wlr_buffer_resource_interface s_TransportResourceIface = {
        .name = "tegra_l4t_buffer",
        .is_instance = tl4t_res_is_instance,
        .from_resource = tl4t_res_from,
    };

    static void tl4t_get_device_uuids( uint8_t *pDevice, uint8_t *pDriver )
    {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &id;
        g_device.vk.GetPhysicalDeviceProperties2( g_device.physDev(), &props );
        memcpy( pDevice, id.deviceUUID, 16 );
        memcpy( pDriver, id.driverUUID, 16 );
    }

    // ------------------------------------------------- registration + import
    static const uint32_t kMaxDim = 7680;
    static const uint32_t kMaxAlloc = 512u * 1024u * 1024u;

    static void tl4t_manager_destroy( ::wl_client *, ::wl_resource *pResource )
    { wl_resource_destroy( pResource ); }

    static void tl4t_manager_register_buffer( ::wl_client *pClient, ::wl_resource *pResource,
        uint32_t uId, int32_t nWidth, int32_t nHeight, uint32_t uDrmFormat,
        uint32_t uBufferSize, uint32_t uBufferUsage, uint32_t uBufferSharing,
        uint32_t uAllocSize, uint32_t uMemoryTypeIndex, uint32_t uMemoryTypeBits,
        uint32_t uMemoryPropertyFlags, uint32_t uDedicated,
        struct wl_array *pDeviceUuid, struct wl_array *pDriverUuid,
        int32_t nMemoryFd, int32_t nReadyFd, int32_t nReturnedFd )
    {
        auto reject = [&]( uint32_t uErr, const char *pMsg ) {
            if ( nMemoryFd >= 0 ) close( nMemoryFd );
            if ( nReadyFd >= 0 ) close( nReadyFd );
            if ( nReturnedFd >= 0 ) close( nReturnedFd );
            wl_resource_post_error( pResource, uErr, "%s", pMsg );
        };

        if ( nWidth <= 0 || nHeight <= 0 || (uint32_t)nWidth > kMaxDim || (uint32_t)nHeight > kMaxDim )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "invalid dimensions" ); return; }
        if ( uDrmFormat != DRM_FORMAT_ABGR8888 )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "only RGBA8 (ABGR8888) in v2" ); return; }
        if ( uBufferSharing != 0 )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "only exclusive sharing" ); return; }
        uint64_t uTightlyPacked = (uint64_t)(uint32_t)nWidth * 4u * (uint32_t)nHeight;
        if ( (uint64_t)uBufferSize != uTightlyPacked )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "buffer_size != width*4*height" ); return; }
        if ( (uBufferUsage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) !=
             (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "buffer_usage missing transfer bits" ); return; }
        if ( uAllocSize < uBufferSize || uAllocSize > kMaxAlloc )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "bad allocation_size" ); return; }
        if ( nMemoryFd < 0 || nReadyFd < 0 || nReturnedFd < 0 )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "missing fd" ); return; }
        if ( pDeviceUuid == nullptr || pDeviceUuid->size != 16 || pDriverUuid == nullptr || pDriverUuid->size != 16 )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "bad uuid length" ); return; }

        uint8_t device[16], driver[16];
        tl4t_get_device_uuids( device, driver );
        if ( memcmp( device, pDeviceUuid->data, 16 ) != 0 || memcmp( driver, pDriverUuid->data, 16 ) != 0 )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "device/driver uuid mismatch" ); return; }

        tl4t_resolve_fns();
        if ( s_pfnGetBufferMemReq2 == nullptr )
        { reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "vkGetBufferMemoryRequirements2 unavailable" ); return; }

        VkDevice dev = g_device.device();

        CTegraL4TBuffer *p = new CTegraL4TBuffer();
        p->width = (uint32_t)nWidth;
        p->height = (uint32_t)nHeight;
        p->drm_format = uDrmFormat;
        p->buffer_size = uBufferSize;
        p->buffer_usage = uBufferUsage;
        p->buffer_sharing = uBufferSharing;
        p->allocation_size = uAllocSize;
        p->memory_type_index = uMemoryTypeIndex;
        p->memory_type_bits = uMemoryTypeBits;
        p->memory_property_flags = uMemoryPropertyFlags;
        p->dedicated = uDedicated;
        memcpy( p->device_uuid, pDeviceUuid->data, 16 );
        memcpy( p->driver_uuid, pDriverUuid->data, 16 );

        // 1. Create the matching buffer (exact size/usage/sharing).
        VkExternalMemoryBufferCreateInfo ext{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.pNext = &ext;
        bci.size = uBufferSize;
        bci.usage = uBufferUsage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ( g_device.vk.CreateBuffer( dev, &bci, nullptr, &p->vkBuffer ) != VK_SUCCESS )
        { delete p; reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "CreateBuffer failed" ); return; }

        // 2. Local requirements + dedicated requirements.
        VkBufferMemoryRequirementsInfo2 reqInfo{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
        reqInfo.buffer = p->vkBuffer;
        VkMemoryDedicatedRequirements dedReq{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
        VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        req2.pNext = &dedReq;
        s_pfnGetBufferMemReq2( dev, &reqInfo, &req2 );

        if ( (uint64_t)req2.memoryRequirements.size > (uint64_t)uAllocSize )
        {
            g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "local requirement > exported allocation" ); return;
        }
        if ( ( req2.memoryRequirements.memoryTypeBits & ( 1u << uMemoryTypeIndex ) ) == 0 ||
             uMemoryTypeIndex >= 32 )
        {
            g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "memoryTypeIndex incompatible" ); return;
        }

        // External (OPAQUE_FD) properties for this usage: export/import/dedicated-only.
        bool bExtImportable = true, bExtDedicatedOnly = false;
        if ( s_pfnGetExtBufProps != nullptr )
        {
            VkPhysicalDeviceExternalBufferInfo ebi{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
            ebi.usage = uBufferUsage;
            ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkExternalBufferProperties eprops{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
            s_pfnGetExtBufProps( g_device.physDev(), &ebi, &eprops );
            auto f = eprops.externalMemoryProperties.externalMemoryFeatures;
            bExtImportable = ( f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ) != 0;
            bExtDedicatedOnly = ( f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ) != 0;
        }
        if ( !bExtImportable ||
             ( bExtDedicatedOnly != ( uDedicated != 0 ) ) ||
             ( dedReq.requiresDedicatedAllocation != ( uDedicated != 0 ) ) )
        {
            g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "external/dedicated inconsistency" ); return;
        }

        // 3. Import the exact memory payload.
        VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imp.fd = nMemoryFd;
        VkMemoryDedicatedAllocateInfo mda{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        mda.buffer = p->vkBuffer;
        if ( uDedicated ) imp.pNext = &mda;
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.pNext = &imp;
        mai.allocationSize = uAllocSize;
        mai.memoryTypeIndex = uMemoryTypeIndex;
        if ( g_device.vk.AllocateMemory( dev, &mai, nullptr, &p->vkMemory ) != VK_SUCCESS )
        {
            g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "import AllocateMemory failed" ); return;
        }
        nMemoryFd = -1; // consumed by the import
        if ( g_device.vk.BindBufferMemory( dev, p->vkBuffer, p->vkMemory, 0 ) != VK_SUCCESS )
        {
            g_device.vk.FreeMemory( dev, p->vkMemory, nullptr );
            g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "BindBufferMemory failed" ); return;
        }

        // 4. Import the ready/returned binary semaphores.
        auto import_sem = [&]( int fd, VkSemaphore *pOut ) -> bool {
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if ( g_device.vk.CreateSemaphore( dev, &sci, nullptr, pOut ) != VK_SUCCESS ) return false;
            VkImportSemaphoreFdInfoKHR isi{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
            isi.semaphore = *pOut;
            isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            isi.fd = fd;
            if ( g_device.vk.ImportSemaphoreFdKHR( dev, &isi ) != VK_SUCCESS )
            { g_device.vk.DestroySemaphore( dev, *pOut, nullptr ); *pOut = VK_NULL_HANDLE; return false; }
            return true;
        };
        if ( !import_sem( nReadyFd, &p->ready ) )
        {
            g_device.vk.FreeMemory( dev, p->vkMemory, nullptr ); g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "ready semaphore import failed" ); return;
        }
        nReadyFd = -1;
        if ( !import_sem( nReturnedFd, &p->returned ) )
        {
            g_device.vk.DestroySemaphore( dev, p->ready, nullptr );
            g_device.vk.FreeMemory( dev, p->vkMemory, nullptr ); g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p;
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_UNSUPPORTED, "returned semaphore import failed" ); return;
        }
        nReturnedFd = -1;

        // 5. Expose as a wl_buffer (real wl_surface/commit object).
        wlr_buffer_init( &p->base, &s_TransportBufferImpl, nWidth, nHeight );
        struct wl_resource *pBufResource = wl_resource_create( pClient, &wl_buffer_interface, 1, uId );
        if ( pBufResource == nullptr )
        {
            reject( TEGRA_L4T_BUFFER_MANAGER_ERROR_BAD_METADATA, "cannot create wl_buffer" );
            // reject closed nothing (all fds consumed); tear the slot down via wlr ref
            p->resource = nullptr;
            wlr_buffer_drop( &p->base );
            return;
        }
        p->resource = pBufResource;
        p->imported = true;
        wl_resource_set_implementation( pBufResource, &s_TransportWlBufferImpl, p, tl4t_wl_buffer_resource_destroy );

        tl4t_log( "register_buffer: %dx%d usg=0x%x size=%u alloc=%u mtIndex=%u mtBits=0x%x mpf=0x%x ded=%u",
                  nWidth, nHeight, uBufferUsage, uBufferSize, uAllocSize, uMemoryTypeIndex,
                  uMemoryTypeBits, uMemoryPropertyFlags, uDedicated );
    }

    static const struct tegra_l4t_buffer_manager_interface s_ManagerImpl = {
        .destroy = tl4t_manager_destroy,
        .register_buffer = tl4t_manager_register_buffer,
    };

    static void tl4t_manager_bind( ::wl_client *pClient, void *, uint32_t uVersion, uint32_t uId )
    {
        ::wl_resource *pResource = wl_resource_create( pClient, &tegra_l4t_buffer_manager_interface,
                                                       uVersion < 1u ? uVersion : 1u, uId );
        if ( pResource == nullptr ) { wl_client_post_no_memory( pClient ); return; }
        wl_resource_set_implementation( pResource, &s_ManagerImpl, nullptr, nullptr );
    }

    void TegraL4TInitTransport( ::wl_display *pDisplay )
    {
        static bool s_bInited = false;
        if ( s_bInited ) return;
        s_bInited = true;
        wlr_buffer_register_resource_interface( &s_TransportResourceIface );
        wl_global_create( pDisplay, &tegra_l4t_buffer_manager_interface, 1, nullptr, tl4t_manager_bind );
        tl4t_log( "transport global registered" );
    }

    // ------------------------------------------------- per-generation consumer copy
    static VkCommandPool s_Pool = VK_NULL_HANDLE;
    static VkCommandBuffer s_Cmd = VK_NULL_HANDLE;
    static VkFence s_Fence = VK_NULL_HANDLE;

    static bool tl4t_gpu_init()
    {
        if ( s_Pool != VK_NULL_HANDLE ) return true;
        VkDevice dev = g_device.device();
        VkCommandPoolCreateInfo cp{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = g_device.queueFamily();
        if ( g_device.vk.CreateCommandPool( dev, &cp, nullptr, &s_Pool ) != VK_SUCCESS )
        { tl4t_log( "consumer CreateCommandPool failed" ); s_Pool = VK_NULL_HANDLE; return false; }
        VkCommandBufferAllocateInfo ca{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ca.commandPool = s_Pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        if ( g_device.vk.AllocateCommandBuffers( dev, &ca, &s_Cmd ) != VK_SUCCESS )
        { tl4t_log( "consumer AllocateCommandBuffers failed" ); return false; }
        VkFenceCreateInfo fc{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if ( g_device.vk.CreateFence( dev, &fc, nullptr, &s_Fence ) != VK_SUCCESS )
        { tl4t_log( "consumer CreateFence failed" ); return false; }
        return true;
    }

    // ------------------------------------------------- disposition completion
    // M7f2j: a received transport commit owns exactly one ready/returned
    // handshake round and must reach exactly one of two outcomes: a copy
    // (TegraL4TCreateTextureFromTransportBuffer) or a final discard that still
    // completes the handshake (below). A silent drop leaves the producer's next
    // reuse of that slot waiting `returned` forever (run #9 defect class).

    // Send wl_buffer.release if the proxy is still alive (Wayland lifetime only;
    // never called while holding a GPU wait).
    static void tl4t_send_release( CTegraL4TBuffer *p )
    {
        wlserver_lock();
        if ( p->resource != nullptr )
            wl_buffer_send_release( p->resource );
        wlserver_unlock();
    }

    static void tl4t_log_disposition( CTegraL4TBuffer *p, const char *pDisposition, const char *pReason )
    {
        tl4t_log( "disposition buf=%p commit=%llu: %s (%s)", (void *)p,
                  (unsigned long long)p->commit_serial, pDisposition, pReason );
    }

    // Complete the ready/returned handshake WITHOUT copying. The GPU sequence is
    // the copy path's ownership dance minus the buffer->image read: wait ready
    // (TRANSFER) -> ACQUIRE EXTERNAL->consumer -> RELEASE consumer->EXTERNAL ->
    // signal returned -> bounded fence. Caller already accounted this generation
    // (commit_serial). Never called under the wlserver lock. Every stage failure
    // fails closed: a half-completed handshake must not be silently abandoned.
    // M7g: the ready/returned "no-copy acknowledgment" dance, parameterized on
    // the raw handshake objects so BOTH the received-transport discard and the
    // EGLStream consumer's exported-slot discard reuse ONE body (unchanged GPU
    // sequence). Callers already accounted their generation.
    static void tl4t_gpu_ack_discard( VkBuffer vkBuffer, VkSemaphore ready, VkSemaphore returned )
    {
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();
        g_device.vk.ResetFences( dev, 1, &s_Fence );
        g_device.vk.ResetCommandBuffer( s_Cmd, 0 );
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( g_device.vk.BeginCommandBuffer( s_Cmd, &bi ) != VK_SUCCESS )
        { tl4t_log( "FATAL: discard BeginCommandBuffer failed" ); fflush( stderr ); _exit( 3 ); }

        // ACQUIRE shared buffer: EXTERNAL -> consumer (same barrier as the copy path).
        VkBufferMemoryBarrier acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        acq.srcAccessMask = 0;
        acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acq.dstQueueFamilyIndex = qfam;
        acq.buffer = vkBuffer; acq.offset = 0; acq.size = VK_WHOLE_SIZE;
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        0, 0, nullptr, 1, &acq, 0, nullptr );

        // RELEASE shared buffer: consumer -> EXTERNAL (same barrier as the copy path).
        VkBufferMemoryBarrier rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rel.dstAccessMask = 0;
        rel.srcQueueFamilyIndex = qfam;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        rel.buffer = vkBuffer; rel.offset = 0; rel.size = VK_WHOLE_SIZE;
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        0, 0, nullptr, 1, &rel, 0, nullptr );

        if ( g_device.vk.EndCommandBuffer( s_Cmd ) != VK_SUCCESS )
        { tl4t_log( "FATAL: discard EndCommandBuffer failed" ); fflush( stderr ); _exit( 3 ); }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &ready; si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1; si.pCommandBuffers = &s_Cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &returned;
        if ( g_device.vk.QueueSubmit( g_device.queue(), 1, &si, s_Fence ) != VK_SUCCESS )
        { tl4t_log( "FATAL: discard QueueSubmit failed" ); fflush( stderr ); _exit( 3 ); }

        if ( g_device.vk.WaitForFences( dev, 1, &s_Fence, VK_TRUE, 2ull * 1000ull * 1000ull * 1000ull ) != VK_SUCCESS )
        {
            // M9W discard fence slow (>2s); waiting up to +8s: under heavy system load the producer can be
            // late by seconds; keep waiting before failing closed.
            tl4t_log( "discard fence slow (>2s); waiting up to +8s" );
            if ( g_device.vk.WaitForFences( dev, 1, &s_Fence, VK_TRUE, 8ull * 1000ull * 1000ull * 1000ull ) != VK_SUCCESS )
            { tl4t_log( "FATAL: discard fence timeout; not recycling pending work" ); fflush( stderr ); _exit( 3 ); }
        }
    }

    static void tl4t_ack_discard( CTegraL4TBuffer *p )
    {
        tl4t_gpu_ack_discard( p->vkBuffer, p->ready, p->returned );
    }

    // Final discard disposition of an already-accounted generation.
    static void tl4t_dispose_discard( CTegraL4TBuffer *p, const char *pReason )
    {
        tl4t_log_disposition( p, "discard", pReason );
        tl4t_ack_discard( p );
        tl4t_send_release( p );
    }

    // M7g: the accepted buffer->image copy body (barriers, region, submit, bounded
    // fence) parameterized on the raw handshake objects. Reused UNCHANGED by the
    // received-transport path and by the EGLStream consumer's exported-slot path.
    // Returns nullptr on a PRE-SUBMIT preparation failure (nothing submitted, so
    // the caller may still complete a discard); on submit/fence failure the queue
    // state is ambiguous and it fails closed with _exit(3), exactly as before.
    static OwningRc<CVulkanTexture> tl4t_gpu_copy_buffer_to_image(
        VkBuffer vkBuffer, uint32_t width, uint32_t height,
        VkSemaphore ready, VkSemaphore returned, const char **ppFailReason,
        const char *origin, const void *record, uint64_t serial )
    {
        *ppFailReason = nullptr;
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();

        OwningRc<CVulkanTexture> pTex = new CVulkanTexture();
        CVulkanTexture::createFlags flags;
        flags.bSampled = true;
        flags.bTransferDst = true;
        if ( !pTex->BInit( width, height, 1u, DRM_FORMAT_ABGR8888, flags, nullptr, width, height ) )
        { tl4t_log( "copy refused: BInit failed" ); *ppFailReason = "BInit"; return nullptr; }

        g_device.vk.ResetFences( dev, 1, &s_Fence );
        g_device.vk.ResetCommandBuffer( s_Cmd, 0 );
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( g_device.vk.BeginCommandBuffer( s_Cmd, &bi ) != VK_SUCCESS )
        { tl4t_log( "copy refused: BeginCommandBuffer" ); *ppFailReason = "BeginCommandBuffer"; return nullptr; }

        // ACQUIRE shared buffer: EXTERNAL -> consumer.
        VkBufferMemoryBarrier acq{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        acq.srcAccessMask = 0;
        acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acq.dstQueueFamilyIndex = qfam;
        acq.buffer = vkBuffer; acq.offset = 0; acq.size = VK_WHOLE_SIZE;
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        0, 0, nullptr, 1, &acq, 0, nullptr );

        // Owned image: UNDEFINED -> GENERAL (transfer write).
        VkImageMemoryBarrier img1{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        img1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        img1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        img1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img1.image = pTex->vkImage();
        img1.srcAccessMask = 0;
        img1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        img1.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        0, 0, nullptr, 0, nullptr, 1, &img1 );

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;   // TEXELS; 0 == tightly packed by width
        region.bufferImageHeight = 0;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { width, height, 1 };
        g_device.vk.CmdCopyBufferToImage( s_Cmd, vkBuffer, pTex->vkImage(),
                                          VK_IMAGE_LAYOUT_GENERAL, 1, &region );

        // image TRANSFER_WRITE -> SHADER_READ.
        VkImageMemoryBarrier img2{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        img2.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        img2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        img2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        img2.image = pTex->vkImage();
        img2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        img2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        img2.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                        0, 0, nullptr, 0, nullptr, 1, &img2 );

        // RELEASE shared buffer: consumer -> EXTERNAL.
        VkBufferMemoryBarrier rel{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rel.dstAccessMask = 0;
        rel.srcQueueFamilyIndex = qfam;
        rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        rel.buffer = vkBuffer; rel.offset = 0; rel.size = VK_WHOLE_SIZE;
        g_device.vk.CmdPipelineBarrier( s_Cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                        0, 0, nullptr, 1, &rel, 0, nullptr );

        if ( g_device.vk.EndCommandBuffer( s_Cmd ) != VK_SUCCESS )
        { tl4t_log( "copy refused: EndCommandBuffer" ); *ppFailReason = "EndCommandBuffer"; return nullptr; }

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &ready; si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1; si.pCommandBuffers = &s_Cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &returned;
        const bool trace = width < 500 || serial < 4 || serial % 120 == 0;
        if ( trace ) tl4t_log( "M10K_COPY_BEGIN origin=%s record=%p serial=%llu size=%ux%u ready=%p returned=%p",
            origin, record, (unsigned long long)serial, width, height, (void*)ready, (void*)returned );
        // M7f2j boundary: after a failed QueueSubmit the queue state is unknown,
        // so the handshake is NOT re-submitted; fail closed instead. (Pre-submit
        // preparation failures above still complete as a discard: nothing was
        // submitted, so the queue is untouched and `ready` is still unconsumed.)
        if ( g_device.vk.QueueSubmit( g_device.queue(), 1, &si, s_Fence ) != VK_SUCCESS )
        { tl4t_log( "FATAL: copy QueueSubmit failed; not retrying the handshake" ); fflush( stderr ); _exit( 3 ); }

        if ( g_device.vk.WaitForFences( dev, 1, &s_Fence, VK_TRUE, 2ull * 1000ull * 1000ull * 1000ull ) != VK_SUCCESS )
        {
            // M9W consumer copy fence slow (>2s); waiting up to +28s: under heavy system load the producer can be
            // late by seconds; keep waiting before failing closed.
            tl4t_log( "M10K_COPY_SLOW origin=%s record=%p serial=%llu size=%ux%u ready=%p returned=%p",
                origin, record, (unsigned long long)serial, width, height, (void*)ready, (void*)returned );
            tl4t_log( "consumer copy fence slow (>2s); waiting up to +28s" );
            if ( g_device.vk.WaitForFences( dev, 1, &s_Fence, VK_TRUE, 28ull * 1000ull * 1000ull * 1000ull ) != VK_SUCCESS )
            { tl4t_log( "FATAL: consumer copy fence timeout; not recycling pending work" ); fflush( stderr ); _exit( 3 ); }
        }

        if ( trace ) tl4t_log( "M10K_COPY_DONE origin=%s record=%p serial=%llu", origin, record, (unsigned long long)serial );
        return pTex;
    }

    OwningRc<CVulkanTexture> TegraL4TCreateTextureFromTransportBuffer( ::wlr_buffer *pBuffer )
    {
        if ( !TegraL4TIsTransportBuffer( pBuffer ) )
            return nullptr;
        CTegraL4TBuffer *p = reinterpret_cast<CTegraL4TBuffer *>( pBuffer );
        if ( !p->imported || p->vkBuffer == VK_NULL_HANDLE || !tl4t_gpu_init() )
        { tl4t_log( "copy refused: slot not ready" ); return nullptr; }

        // M7f2j: this received commit event now owns exactly one disposition round.
        p->commit_serial++;

        const char *pFailReason = nullptr;
        OwningRc<CVulkanTexture> pTex = tl4t_gpu_copy_buffer_to_image(
            p->vkBuffer, p->width, p->height, p->ready, p->returned, &pFailReason, "raw", p, p->commit_serial );
        if ( pTex == nullptr )
        {
            char reason[64];
            snprintf( reason, sizeof(reason), "copy-failed:%s", pFailReason ? pFailReason : "?" );
            tl4t_dispose_discard( p, reason );
            return nullptr;
        }

        // GPU copy is complete and ownership returned: now it is safe to tell the
        // producer (Wayland lifetime only). The release event goes out under the
        // lock after the GPU wait, never while holding one.
        tl4t_send_release( p );

        // Same throttle as before (first 3 and every 4th), now per received commit.
        if ( p->commit_serial <= 3 || ( p->commit_serial % 4 ) == 0 )
            tl4t_log_disposition( p, "copy", "ok" );

        return pTex;
    }

    void TegraL4TDiscardTransportCommit( ::wlr_buffer *pBuffer, const char *pReason )
    {
        if ( !TegraL4TIsTransportBuffer( pBuffer ) )
            return;
        CTegraL4TBuffer *p = reinterpret_cast<CTegraL4TBuffer *>( pBuffer );
        if ( !p->imported || p->vkBuffer == VK_NULL_HANDLE || !tl4t_gpu_init() )
        { tl4t_log( "discard refused: slot not ready (handshake left incomplete)" ); return; }

        // This received commit event now owns exactly one disposition round.
        p->commit_serial++;
        tl4t_dispose_discard( p, pReason );
    }

    // =====================================================================
    // M7g - compositor-produced EXPORTED transport slot (the "one internal
    // create-and-export entry" of decision 5). Same OPAQUE_FD + semaphore
    // discipline as the reviewed exchange, but the compositor OWNS the memory:
    // the consumer thread imports mem_fd/ready_fd/returned_fd into GL, writes
    // the acquired EGLStream frame into it and signals `ready`; the compositor
    // waits `ready`, copies it with the accepted buffer->image body and signals
    // `returned`. The received-transport object CTegraL4TBuffer is untouched.
    // =====================================================================

    CTegraL4TExportedSlot *TegraL4TCreateExportedSlot( uint32_t uWidth, uint32_t uHeight )
    {
        if ( uWidth == 0 || uHeight == 0 || uWidth > kMaxDim || uHeight > kMaxDim )
        { tl4t_log( "slot: bad extent %ux%u", uWidth, uHeight ); return nullptr; }
        uint64_t uSize64 = (uint64_t)uWidth * 4u * (uint64_t)uHeight;
        if ( uSize64 == 0 || uSize64 > (uint64_t)kMaxAlloc )
        { tl4t_log( "slot: size %llu out of range", (unsigned long long)uSize64 ); return nullptr; }

        tl4t_resolve_fns();
        VkDevice dev = g_device.device();
        if ( dev == VK_NULL_HANDLE || s_pfnGetBufferMemReq2 == nullptr )
        { tl4t_log( "slot: device/unavailable" ); return nullptr; }

        uint32_t uSize = (uint32_t)uSize64;
        CTegraL4TExportedSlot *p = new CTegraL4TExportedSlot();
        p->width = uWidth; p->height = uHeight; p->allocation_size = uSize;

        VkExternalMemoryBufferCreateInfo ext{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.pNext = &ext;
        bci.size = uSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ( g_device.vk.CreateBuffer( dev, &bci, nullptr, &p->vkBuffer ) != VK_SUCCESS )
        { tl4t_log( "slot: CreateBuffer failed" ); delete p; return nullptr; }

        VkBufferMemoryRequirementsInfo2 reqInfo{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
        reqInfo.buffer = p->vkBuffer;
        VkMemoryDedicatedRequirements dedReq{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
        VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
        req2.pNext = &dedReq;
        s_pfnGetBufferMemReq2( dev, &reqInfo, &req2 );
        uint32_t uMtBits = req2.memoryRequirements.memoryTypeBits;
        uint32_t uReqSize = (uint32_t)req2.memoryRequirements.size;

        VkPhysicalDeviceMemoryProperties mp;
        g_device.vk.GetPhysicalDeviceMemoryProperties( g_device.physDev(), &mp );
        uint32_t uMtIndex = UINT32_MAX;
        for ( uint32_t i = 0; i < mp.memoryTypeCount; i++ )
        {
            if ( ( uMtBits & ( 1u << i ) ) == 0 ) continue;
            if ( mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ) { uMtIndex = i; break; }
            if ( uMtIndex == UINT32_MAX ) uMtIndex = i;
        }
        if ( uMtIndex == UINT32_MAX )
        { tl4t_log( "slot: no memory type (bits=0x%x)", uMtBits );
          g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p; return nullptr; }
        if ( uReqSize > uSize ) uSize = uReqSize;
        p->allocation_size = uSize;
        p->memory_type_index = uMtIndex;
        p->memory_type_bits = uMtBits;
        p->memory_property_flags = (uint32_t)mp.memoryTypes[uMtIndex].propertyFlags;

        bool bDedicatedOnly = false;
        if ( s_pfnGetExtBufProps != nullptr )
        {
            VkPhysicalDeviceExternalBufferInfo ebi{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
            ebi.usage = bci.usage;
            ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkExternalBufferProperties eprops{ VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
            s_pfnGetExtBufProps( g_device.physDev(), &ebi, &eprops );
            bDedicatedOnly = ( eprops.externalMemoryProperties.externalMemoryFeatures &
                               VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ) != 0;
        }
        p->dedicated = ( dedReq.requiresDedicatedAllocation || bDedicatedOnly ) ? 1u : 0u;
        tl4t_get_device_uuids( p->device_uuid, p->driver_uuid );

        VkMemoryDedicatedAllocateInfo mda{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        mda.buffer = p->vkBuffer;
        VkExportMemoryAllocateInfo exa{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        exa.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if ( p->dedicated ) exa.pNext = &mda;
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.pNext = &exa;
        mai.allocationSize = uSize;
        mai.memoryTypeIndex = uMtIndex;
        if ( g_device.vk.AllocateMemory( dev, &mai, nullptr, &p->vkMemory ) != VK_SUCCESS )
        { tl4t_log( "slot: AllocateMemory failed" );
          g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p; return nullptr; }
        if ( g_device.vk.BindBufferMemory( dev, p->vkBuffer, p->vkMemory, 0 ) != VK_SUCCESS )
        { tl4t_log( "slot: BindBufferMemory failed" );
          g_device.vk.FreeMemory( dev, p->vkMemory, nullptr );
          g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p; return nullptr; }

        VkMemoryGetFdInfoKHR gfi{ VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        gfi.memory = p->vkMemory;
        gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if ( g_device.vk.GetMemoryFdKHR( dev, &gfi, &p->mem_fd ) != VK_SUCCESS )
        { tl4t_log( "slot: GetMemoryFdKHR failed" );
          g_device.vk.FreeMemory( dev, p->vkMemory, nullptr );
          g_device.vk.DestroyBuffer( dev, p->vkBuffer, nullptr ); delete p; return nullptr; }

        VkExportSemaphoreCreateInfo esc{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        esc.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sci.pNext = &esc;
        VkSemaphore *pSems[2] = { &p->ready, &p->returned };
        int *pFds[2] = { &p->ready_fd, &p->returned_fd };
        for ( int i = 0; i < 2; i++ )
        {
            if ( g_device.vk.CreateSemaphore( dev, &sci, nullptr, pSems[i] ) != VK_SUCCESS )
            { tl4t_log( "slot: CreateSemaphore[%d] failed", i ); TegraL4TFreeExportedSlot( p ); return nullptr; }
            VkSemaphoreGetFdInfoKHR sgi{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
            sgi.semaphore = *pSems[i];
            sgi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
            if ( g_device.vk.GetSemaphoreFdKHR( dev, &sgi, pFds[i] ) != VK_SUCCESS )
            { tl4t_log( "slot: GetSemaphoreFdKHR[%d] failed", i ); TegraL4TFreeExportedSlot( p ); return nullptr; }
        }

        tl4t_log( "slot: %ux%u alloc=%u mtIndex=%u ded=%u mem_fd=%d ready_fd=%d returned_fd=%d",
                  uWidth, uHeight, p->allocation_size, p->memory_type_index, p->dedicated,
                  p->mem_fd, p->ready_fd, p->returned_fd );
        return p;
    }

    bool TegraL4TGetSlotImportInfo( CTegraL4TExportedSlot *pSlot, TegraL4TSlotImportInfo *pOut )
    {
        if ( pSlot == nullptr || pOut == nullptr ) return false;
        pOut->mem_fd = pSlot->mem_fd;
        pOut->ready_fd = pSlot->ready_fd;
        pOut->returned_fd = pSlot->returned_fd;
        pOut->allocation_size = pSlot->allocation_size;
        pOut->memory_type_index = pSlot->memory_type_index;
        pOut->memory_type_bits = pSlot->memory_type_bits;
        pOut->memory_property_flags = pSlot->memory_property_flags;
        pOut->dedicated = pSlot->dedicated;
        memcpy( pOut->device_uuid, pSlot->device_uuid, 16 );
        memcpy( pOut->driver_uuid, pSlot->driver_uuid, 16 );
        pOut->width = pSlot->width; pOut->height = pSlot->height;
        return pOut->mem_fd >= 0 && pOut->ready_fd >= 0 && pOut->returned_fd >= 0;
    }

    OwningRc<CVulkanTexture> TegraL4TFetchSlotToTexture( CTegraL4TExportedSlot *pSlot )
    {
        if ( pSlot == nullptr || pSlot->vkBuffer == VK_NULL_HANDLE || !tl4t_gpu_init() )
            return nullptr;
        const char *pFail = nullptr;
        OwningRc<CVulkanTexture> pTex = tl4t_gpu_copy_buffer_to_image(
            pSlot->vkBuffer, pSlot->width, pSlot->height, pSlot->ready, pSlot->returned, &pFail, "egls-exported", pSlot, 0 );
        if ( pTex == nullptr )
        {
            tl4t_log( "slot copy failed: %s (completing handshake as discard)", pFail ? pFail : "?" );
            tl4t_gpu_ack_discard( pSlot->vkBuffer, pSlot->ready, pSlot->returned );
        }
        return pTex;
    }

    void TegraL4TAckSlotDiscard( CTegraL4TExportedSlot *pSlot )
    {
        if ( pSlot == nullptr || pSlot->vkBuffer == VK_NULL_HANDLE || !tl4t_gpu_init() ) return;
        tl4t_gpu_ack_discard( pSlot->vkBuffer, pSlot->ready, pSlot->returned );
    }

    void TegraL4TFreeExportedSlot( CTegraL4TExportedSlot *pSlot )
    {
        if ( pSlot == nullptr ) return;
        VkDevice dev = g_device.device();
        if ( dev != VK_NULL_HANDLE )
        {
            if ( pSlot->ready )    g_device.vk.DestroySemaphore( dev, pSlot->ready, nullptr );
            if ( pSlot->returned ) g_device.vk.DestroySemaphore( dev, pSlot->returned, nullptr );
            if ( pSlot->vkBuffer ) g_device.vk.DestroyBuffer( dev, pSlot->vkBuffer, nullptr );
            if ( pSlot->vkMemory ) g_device.vk.FreeMemory( dev, pSlot->vkMemory, nullptr );
        }
        if ( pSlot->mem_fd >= 0 ) close( pSlot->mem_fd );
        if ( pSlot->ready_fd >= 0 ) close( pSlot->ready_fd );
        if ( pSlot->returned_fd >= 0 ) close( pSlot->returned_fd );
        delete pSlot;
    }
}

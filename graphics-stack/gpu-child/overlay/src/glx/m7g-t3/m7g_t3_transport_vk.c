/* M7g T3 — real transport backend (generalized from the accepted T2/T1
 * OPAQUE_FD transport; ONLY the symbol/log prefix and per-instance SIZE
 * parameterization differ: slots/staging/register_buffer use the actual
 * drawable w/h instead of a fixed 1280x720.  The slot/ready/returned/
 * external-ownership/lifetime/backpressure contract is unchanged).
 *
 * Producer bridge: CPU pixels (Zink FBO readback, machine-checked) are copied
 * into a per-slot host-visible staging buffer, then copied on the bridge
 * VkDevice/queue into the accepted-exportable OPAQUE_FD VkBuffer slot (exact
 * reference-client pattern: immutable registration, exported memory +
 * ready/returned binary semaphores, external ownership barriers, bounded
 * reuse, release after copy fence on the compositor side).
 *
 * No client NVIDIA WSI anywhere: no VkSurface/swapchain/present; the only
 * Wayland objects are the wl_surface carrying transport buffers and the
 * gamescope swapchain/override handles.
 */
#include "m7g_t3_transport.h"
#include "m9w-gpu.h"
#include "release-gate.h"
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#include <vulkan/vulkan.h>
#include <wayland-client.h>

#include "gamescope-swapchain-client-protocol.h"
#include "tegra-l4t-buffer-client-protocol.h"

#define M7G_T3_SLOTS 3u
#define M7G_T3_FENCE_WAIT_NS (3000000000ull)
#define M7G_T3_DRM_FORMAT_ABGR8888 0x34324241u /* DRM_FORMAT_ABGR8888 */

struct t1_slot {
    VkBuffer        buffer;
    VkDeviceMemory  bufferMem;
    uint32_t        allocSize;
    uint32_t        memoryTypeIndex;
    uint32_t        memoryTypeBits;
    uint32_t        memoryPropertyFlags;
    uint32_t        dedicated;
    VkBuffer        staging;
    VkDeviceMemory  stagingMem;
    unsigned char  *stagingMap;
    VkSemaphore     ready;
    VkSemaphore     returned;
    VkFence         fence;
    VkCommandBuffer cmd;
    struct wl_buffer *wlBuffer;
    int             memFd;
    int             readyFd;
    int             returnedFd;
    bool            usedOnce;
    struct m10k_release_state release;
};

struct m7g_t3_xport {
    struct wl_display                    *display;
    struct wl_registry                   *registry;
    struct wl_compositor                 *compositor;
    struct gamescope_swapchain_factory_v2 *factory;
    struct tegra_l4t_buffer_manager      *manager;
    struct wl_surface                    *surface;
    struct gamescope_swapchain           *swapchain;

    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         qfam;
    uint32_t         bridge_device_index;
    char             device_name[256];
    uint8_t          deviceUuid[16];
    uint8_t          driverUuid[16];
    VkPhysicalDeviceMemoryProperties memProps;
    PFN_vkGetMemoryFdKHR    getMemoryFd;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd;
    VkCommandPool    pool;

    struct t1_slot   slot[M7G_T3_SLOTS];
    int              gpu_producer;
    int              m10k_gate;
    unsigned         slotCount;
    unsigned         nextSlot;
    uint32_t         xid;
    uint32_t         server_id;
    uint32_t         w;
    uint32_t         h;
    uint32_t         bytes;
    int              retired;
    m7g_t3_xport_counters c;
};

const char *m7g_t3_xport_backend(void) { return "opaque_fd"; }
uint32_t m7g_t3_xport_w(const m7g_t3_xport *x) { return x ? x->w : 0; }
uint32_t m7g_t3_xport_h(const m7g_t3_xport *x) { return x ? x->h : 0; }

/* ------------------------------------------------------------------ helpers */
static bool choose_memory(const struct m7g_t3_xport *x, uint32_t typeBits,
                          VkMemoryPropertyFlags want, uint32_t *out)
{
    for (uint32_t i = 0; i < x->memProps.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) &&
            (x->memProps.memoryTypes[i].propertyFlags & want) == want) {
            *out = i;
            return true;
        }
    return false;
}

#ifdef M7G_TEST_HOOKS
static int fail_at(const char *stage)
{
    const char *e = getenv("M7G_TEST_FAIL_AT");
    if (e && *e && strcmp(e, stage) == 0) {
        printf("T3_TEST fail_at=%s (injected)\n", stage);
        return 1;
    }
    return 0;
}
#endif

/* ------------------------------------------------------- swapchain events */
static void sc_past_present(void *data, struct gamescope_swapchain *sc,
                            uint32_t present_id,
                            uint32_t desired_hi, uint32_t desired_lo,
                            uint32_t actual_hi, uint32_t actual_lo,
                            uint32_t earliest_hi, uint32_t earliest_lo,
                            uint32_t margin_hi, uint32_t margin_lo)
{
    struct m7g_t3_xport *x = data;
    (void)sc;
    x->c.past_present_timing++;
    if(x->gpu_producer) m9w_log("GPU_PRESENT xid=0x%x w=%u h=%u id=%u actual_ns=%llu",x->xid,x->w,x->h,present_id,(unsigned long long)(((uint64_t)actual_hi<<32)|actual_lo));
    printf("T3_SC past_present id=%u desired=%llu actual=%llu earliest=%llu margin=%llu\n",
           present_id,
           (unsigned long long)(((uint64_t)desired_hi << 32) | desired_lo),
           (unsigned long long)(((uint64_t)actual_hi << 32) | actual_lo),
           (unsigned long long)(((uint64_t)earliest_hi << 32) | earliest_lo),
           (unsigned long long)(((uint64_t)margin_hi << 32) | margin_lo));
}
static void sc_refresh_cycle(void *data, struct gamescope_swapchain *sc,
                             uint32_t hi, uint32_t lo)
{
    struct m7g_t3_xport *x = data;
    (void)sc;
    x->c.refresh_cycle++;
    printf("T3_SC refresh_cycle=%llu\n",
           (unsigned long long)(((uint64_t)hi << 32) | lo));
}
static void sc_retired(void *data, struct gamescope_swapchain *sc)
{
    struct m7g_t3_xport *x = data;
    (void)sc;
    x->retired = 1;
    x->c.retired++;
    printf("T3_SC retired=1 (swapchain remotely retired; no further commits)\n");
}
static const struct gamescope_swapchain_listener sc_listener = {
    .past_present_timing = sc_past_present,
    .refresh_cycle = sc_refresh_cycle,
    .retired = sc_retired,
};

/* ------------------------------------------------------------- wayland in */
static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version)
{
    struct m7g_t3_xport *x = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0 && !x->compositor)
        x->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                         version < 4u ? version : 4u);
    else if (strcmp(iface, gamescope_swapchain_factory_v2_interface.name) == 0 && !x->factory)
        x->factory = wl_registry_bind(reg, name, &gamescope_swapchain_factory_v2_interface, 1u);
    else if (strcmp(iface, tegra_l4t_buffer_manager_interface.name) == 0 && !x->manager)
        x->manager = wl_registry_bind(reg, name, &tegra_l4t_buffer_manager_interface, 1u);
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{ (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

/* --------------------------------------------------------------- vk bridge */
static int vk_bridge_init(struct m7g_t3_xport *x)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "m7g-zink-t3";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &x->instance) != VK_SUCCESS) {
        printf("T3_FAIL vk-instance\n");
        return M7G_T3_XS_INIT;
    }
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(x->instance, &n, NULL) != VK_SUCCESS || n == 0) {
        printf("T3_FAIL vk-no-devices\n");
        return M7G_T3_XS_INIT;
    }
    VkPhysicalDevice devs[8];
    uint32_t want = n < 8u ? n : 8u;
    if (vkEnumeratePhysicalDevices(x->instance, &want, devs) != VK_SUCCESS || want == 0) {
        printf("T3_FAIL vk-enum-devices\n");
        return M7G_T3_XS_INIT;
    }
    x->phys = devs[0];
    x->bridge_device_index = 0;

    VkPhysicalDeviceIDProperties idProp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &idProp;
    vkGetPhysicalDeviceProperties2(x->phys, &props2);
    memcpy(x->deviceUuid, idProp.deviceUUID, 16);
    memcpy(x->driverUuid, idProp.driverUUID, 16);
    snprintf(x->device_name, sizeof(x->device_name), "%s", props2.properties.deviceName);
    vkGetPhysicalDeviceMemoryProperties(x->phys, &x->memProps);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(x->phys, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    uint32_t qwant = qn < 16u ? qn : 16u;
    vkGetPhysicalDeviceQueueFamilyProperties(x->phys, &qwant, qp);
    bool found = false;
    for (uint32_t i = 0; i < qwant; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { x->qfam = i; found = true; break; }
    if (!found) {
        printf("T3_FAIL vk-no-graphics-queue\n");
        return M7G_T3_XS_INIT;
    }

    const char *extNeed[2] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                               VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME };
    uint32_t en = 0;
    if (vkEnumerateDeviceExtensionProperties(x->phys, NULL, &en, NULL) != VK_SUCCESS || en == 0) {
        printf("T3_FAIL vk-ext-enum\n");
        return M7G_T3_XS_INIT;
    }
    VkExtensionProperties exts[512];
    uint32_t ew = en < 512u ? en : 512u;
    vkEnumerateDeviceExtensionProperties(x->phys, NULL, &ew, exts);
    for (unsigned k = 0; k < 2; k++) {
        bool have = false;
        for (uint32_t i = 0; i < ew; i++)
            if (strcmp(exts[i].extensionName, extNeed[k]) == 0) { have = true; break; }
        if (!have) {
            printf("T3_FAIL vk-ext-missing %s\n", extNeed[k]);
            return M7G_T3_XS_INIT;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = x->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats;
    memset(&feats, 0, sizeof(feats));
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = extNeed;
    if (vkCreateDevice(x->phys, &dci, NULL, &x->device) != VK_SUCCESS) {
        printf("T3_FAIL vk-device\n");
        return M7G_T3_XS_INIT;
    }
    vkGetDeviceQueue(x->device, x->qfam, 0, &x->queue);
    x->getMemoryFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(x->device, "vkGetMemoryFdKHR");
    x->getSemaphoreFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(x->device, "vkGetSemaphoreFdKHR");
    if (!x->getMemoryFd || !x->getSemaphoreFd) {
        printf("T3_FAIL vk-ext-entrypoints\n");
        return M7G_T3_XS_INIT;
    }

    VkCommandPoolCreateInfo cp = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = x->qfam;
    if (vkCreateCommandPool(x->device, &cp, NULL, &x->pool) != VK_SUCCESS) {
        printf("T3_FAIL vk-cmd-pool\n");
        return M7G_T3_XS_INIT;
    }

    printf("T3_BRIDGE device_index=%u device=\"%s\" qfam=%u\n",
           x->bridge_device_index, x->device_name, x->qfam);
    printf("T3_BRIDGE device_uuid=");
    for (int i = 0; i < 16; i++) printf("%02x", x->deviceUuid[i]);
    printf(" driver_uuid=");
    for (int i = 0; i < 16; i++) printf("%02x", x->driverUuid[i]);
    printf("\n");
    return 0;
}

/* ------------------------------------------------------------------- slots */
static void buf_release(void *data, struct wl_buffer *wl_buffer)
{
    struct m7g_t3_xport *x = data;
    (void)wl_buffer;
    if ((x->w<500 && x->h>400) || x->c.releases<16 || x->c.releases%120==0) {
        for(unsigned i=0;i<x->slotCount;i++) if(x->slot[i].wlBuffer==wl_buffer)
            m9w_log("M10K_WL_RELEASE xid=0x%x slot=%u buffer=%u surface=%u releases_before=%u",
                    x->xid,i,wl_proxy_get_id((struct wl_proxy*)wl_buffer),wl_proxy_get_id((struct wl_proxy*)x->surface),x->c.releases);
    }
    if(x->m10k_gate)for(unsigned i=0;i<x->slotCount;i++)if(x->slot[i].wlBuffer==wl_buffer){
        struct m10k_release_state *r=&x->slot[i].release;
        int fresh=m10k_release_received(r);
        if(!fresh || (x->w<500 && x->h>400) || r->serial<=8 || r->serial%120==0)
            m9w_log("M10K_RELEASE xid=0x%x slot=%u serial=%llu fresh=%d duplicates=%llu",x->xid,i,(unsigned long long)r->serial,fresh,(unsigned long long)r->duplicates);
        break;
    }
    x->c.releases++;
    printf("T3_RELEASE count=%u\n", x->c.releases);
}
static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static int slot_create(struct m7g_t3_xport *x, unsigned i)
{
    struct t1_slot *s = &x->slot[i];
    const uint32_t usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkResult res;

    VkPhysicalDeviceExternalBufferInfo ebi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO };
    ebi.usage = usage;
    ebi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalBufferProperties eprops = { VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    vkGetPhysicalDeviceExternalBufferProperties(x->phys, &ebi, &eprops);
    VkExternalMemoryFeatureFlags ef = eprops.externalMemoryProperties.externalMemoryFeatures;
    if (!(ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        printf("T3_FAIL slot%u export-unsupported\n", i);
        return M7G_T3_XS_SLOTS;
    }
    const bool extDedicatedOnly = (ef & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;

    VkExternalMemoryBufferCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.pNext = &ext;
    bci.size = x->bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((res = vkCreateBuffer(x->device, &bci, NULL, &s->buffer)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u create-buffer res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }

    VkBufferMemoryRequirementsInfo2 ri = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2 };
    ri.buffer = s->buffer;
    VkMemoryDedicatedRequirements mdr = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 mr2 = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    mr2.pNext = &mdr;
    vkGetBufferMemoryRequirements2(x->device, &ri, &mr2);
    const bool dedicated = mdr.requiresDedicatedAllocation || extDedicatedOnly;
    if (!choose_memory(x, mr2.memoryRequirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s->memoryTypeIndex)) {
        printf("T3_FAIL slot%u no-device-local-type\n", i);
        return M7G_T3_XS_SLOTS;
    }
    s->allocSize = (uint32_t)mr2.memoryRequirements.size;
    s->memoryTypeBits = mr2.memoryRequirements.memoryTypeBits;
    s->memoryPropertyFlags = x->memProps.memoryTypes[s->memoryTypeIndex].propertyFlags;
    s->dedicated = dedicated ? 1u : 0u;

    VkExportMemoryAllocateInfo exa = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exa.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo mda = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    mda.buffer = s->buffer;
    if (dedicated)
        exa.pNext = &mda;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &exa;
    mai.allocationSize = s->allocSize;
    mai.memoryTypeIndex = s->memoryTypeIndex;
    if ((res = vkAllocateMemory(x->device, &mai, NULL, &s->bufferMem)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u alloc res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    if ((res = vkBindBufferMemory(x->device, s->buffer, s->bufferMem, 0)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u bind res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = s->bufferMem;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (x->getMemoryFd(x->device, &gfi, &s->memFd) != VK_SUCCESS) {
        printf("T3_FAIL slot%u get-mem-fd\n", i);
        return M7G_T3_XS_SLOTS;
    }

    if (!x->gpu_producer) {
    /* staging (host-visible, not exported): the only CPU-write target */
    VkBufferCreateInfo sci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    sci.size = x->bytes;
    sci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    sci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if ((res = vkCreateBuffer(x->device, &sci, NULL, &s->staging)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u staging res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    VkMemoryRequirements sr;
    vkGetBufferMemoryRequirements(x->device, s->staging, &sr);
    uint32_t sti = 0;
    if (!choose_memory(x, sr.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sti)) {
        printf("T3_FAIL slot%u no-host-visible-type\n", i);
        return M7G_T3_XS_SLOTS;
    }
    VkMemoryAllocateInfo smai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    smai.allocationSize = sr.size;
    smai.memoryTypeIndex = sti;
    if ((res = vkAllocateMemory(x->device, &smai, NULL, &s->stagingMem)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u staging-alloc res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    if ((res = vkBindBufferMemory(x->device, s->staging, s->stagingMem, 0)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u staging-bind res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    if ((res = vkMapMemory(x->device, s->stagingMem, 0, VK_WHOLE_SIZE, 0, (void **)&s->stagingMap)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u staging-map res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }

    }

    /* ready/returned binary semaphores, exported OPAQUE_FD */
    for (int k = 0; k < 2; k++) {
        VkExportSemaphoreCreateInfo esc = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        esc.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkSemaphoreCreateInfo semci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        semci.pNext = &esc;
        VkSemaphore sem = VK_NULL_HANDLE;
        if ((res = vkCreateSemaphore(x->device, &semci, NULL, &sem)) != VK_SUCCESS) {
            printf("T3_FAIL slot%u semaphore res=%d\n", i, (int)res);
            return M7G_T3_XS_SLOTS;
        }
        VkSemaphoreGetFdInfoKHR sgfi = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
        sgfi.semaphore = sem;
        sgfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        int fd = -1;
        if (x->getSemaphoreFd(x->device, &sgfi, &fd) != VK_SUCCESS) {
            printf("T3_FAIL slot%u semaphore-fd\n", i);
            return M7G_T3_XS_SLOTS;
        }
        if (k == 0) { s->ready = sem; s->readyFd = fd; }
        else        { s->returned = sem; s->returnedFd = fd; }
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if ((res = vkCreateFence(x->device, &fci, NULL, &s->fence)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u fence res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = x->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((res = vkAllocateCommandBuffers(x->device, &cai, &s->cmd)) != VK_SUCCESS) {
        printf("T3_FAIL slot%u cmd res=%d\n", i, (int)res);
        return M7G_T3_XS_SLOTS;
    }

    struct wl_array devUuid, drvUuid;
    wl_array_init(&devUuid);
    wl_array_init(&drvUuid);
    memcpy(wl_array_add(&devUuid, 16), x->deviceUuid, 16);
    memcpy(wl_array_add(&drvUuid, 16), x->driverUuid, 16);
    s->wlBuffer = tegra_l4t_buffer_manager_register_buffer(
        x->manager, (int32_t)x->w, (int32_t)x->h, M7G_T3_DRM_FORMAT_ABGR8888,
        x->bytes, usage, 0u, s->allocSize, s->memoryTypeIndex,
        s->memoryTypeBits, s->memoryPropertyFlags, s->dedicated,
        &devUuid, &drvUuid, s->memFd, s->readyFd, s->returnedFd);
    wl_array_release(&devUuid);
    wl_array_release(&drvUuid);
    if (!s->wlBuffer) {
        printf("T3_FAIL slot%u register-null\n", i);
        return M7G_T3_XS_SLOTS;
    }
    wl_buffer_add_listener(s->wlBuffer, &buf_listener, x);
    printf("T3_SLOT idx=%u alloc=%u mt=%u bits=0x%x mpf=0x%x ded=%u memFd=%d readyFd=%d returnedFd=%d\n",
           i, s->allocSize, s->memoryTypeIndex, s->memoryTypeBits, s->memoryPropertyFlags,
           s->dedicated, s->memFd, s->readyFd, s->returnedFd);
    return 0;
}

/* -------------------------------------------------------------------- init */
int m7g_t3_xport_init(m7g_t3_xport **out, uint32_t server_id, uint32_t xid,
                      uint32_t w, uint32_t h)
{
    if (w < 16u || h < 16u || w > 4096u || h > 4096u ||
        (uint64_t)w * (uint64_t)h * 4ull > (64ull << 20)) {
        printf("T3_FAIL drawable-size-unsupported w=%u h=%u\n", w, h);
        return M7G_T3_XS_INIT;
    }
    struct m7g_t3_xport *x = calloc(1, sizeof(*x));
    if (!x)
        return M7G_T3_XS_INIT;
    x->gpu_producer = getenv("M9W_GPU_CHILD") && !strcmp(getenv("M9W_GPU_CHILD"),"1");
    for (unsigned i=0;i<M7G_T3_SLOTS;i++) x->slot[i].memFd=x->slot[i].readyFd=x->slot[i].returnedFd=-1;
    x->slotCount = M7G_T3_SLOTS;
    x->xid = xid;
    x->w = w;
    x->h = h;
    x->bytes = w * h * 4u;
    x->server_id = server_id;
    int rc;

    const char *name = getenv("GAMESCOPE_WAYLAND_DISPLAY");
    if (!name || !*name)
        name = getenv("WAYLAND_DISPLAY");
    if (!name || !*name) {
        printf("T3_FAIL wayland-display-env-missing (GAMESCOPE_WAYLAND_DISPLAY/WAYLAND_DISPLAY)\n");
        rc = M7G_T3_XS_INIT;
        goto fail;
    }
    x->display = wl_display_connect(name);
    if (!x->display) {
        printf("T3_FAIL wl_display_connect name=%s\n", name);
        rc = M7G_T3_XS_INIT;
        goto fail;
    }
    printf("T3_WL display=%s ok=1\n", name);

#ifdef M7G_TEST_HOOKS
    if (fail_at("init")) { rc = M7G_T3_XS_INIT; goto fail; }
#endif

    x->registry = wl_display_get_registry(x->display);
    wl_registry_add_listener(x->registry, &registry_listener, x);
    if (wl_display_roundtrip(x->display) < 0) {
        printf("T3_FAIL wl-registry-roundtrip\n");
        rc = M7G_T3_XS_BIND;
        goto fail;
    }
    if (!x->compositor || !x->factory || !x->manager) {
        printf("T3_FAIL wl-globals compositor=%d factory=%d manager=%d\n",
               x->compositor ? 1 : 0, x->factory ? 1 : 0, x->manager ? 1 : 0);
        rc = M7G_T3_XS_BIND;
        goto fail;
    }
    printf("T3_BIND compositor=1 factory=1 manager=1\n");
#ifdef M7G_TEST_HOOKS
    if (fail_at("bind")) { rc = M7G_T3_XS_BIND; goto fail; }
#endif

    x->surface = wl_compositor_create_surface(x->compositor);
    if (!x->surface) {
        printf("T3_FAIL wl-surface\n");
        rc = M7G_T3_XS_STAGE;
        goto fail;
    }
    x->swapchain = gamescope_swapchain_factory_v2_create_swapchain(x->factory, x->surface);
    if (!x->swapchain) {
        printf("T3_FAIL wl-swapchain-object\n");
        rc = M7G_T3_XS_STAGE;
        goto fail;
    }
    /* Events (refresh_cycle / past_present_timing / retired) must be handled
     * from creation time; the listener is registered before any dispatch. */
    gamescope_swapchain_add_listener(x->swapchain, &sc_listener, x);
    gamescope_swapchain_override_window_content(x->swapchain, server_id, xid);
    if (wl_display_roundtrip(x->display) < 0 || wl_display_get_error(x->display) != 0) {
        printf("T3_FAIL override-roundtrip err=%d\n", wl_display_get_error(x->display));
        rc = M7G_T3_XS_STAGE;
        goto fail;
    }
    printf("T3_OVERRIDE server_id=%u xid=0x%x ok=1\n", server_id, xid);
#ifdef M7G_TEST_HOOKS
    if (fail_at("stage")) { rc = M7G_T3_XS_STAGE; goto fail; }
#endif

    if ((rc = vk_bridge_init(x)) != 0)
        goto fail;

    for (unsigned i = 0; i < x->slotCount; i++) {
        if ((rc = slot_create(x, i)) != 0)
            goto fail;
        x->c.slots++;
    }
    /* flush registrations and let the server import the fds before we close ours */
    wl_display_flush(x->display);
    if (wl_display_roundtrip(x->display) < 0) {
        printf("T3_FAIL register-roundtrip\n");
        rc = M7G_T3_XS_SLOTS;
        goto fail;
    }
    for (unsigned i = 0; i < x->slotCount; i++) {
        struct t1_slot *s = &x->slot[i];
        if (s->memFd >= 0)      { close(s->memFd);      s->memFd = -1; }
        if (s->readyFd >= 0)    { close(s->readyFd);    s->readyFd = -1; }
        if (s->returnedFd >= 0) { close(s->returnedFd); s->returnedFd = -1; }
    }
    printf("T3_XPORT backend=opaque_fd slots=%u display=%s w=%u h=%u bytes=%u\n",
           x->slotCount, name, x->w, x->h, x->bytes);
    *out = x;
    return 0;

fail:
    m7g_t3_xport_destroy(x);
    return rc;
}

/* ------------------------------------------------------------------ commit */
int m7g_t3_xport_commit(m7g_t3_xport *x, unsigned slot, unsigned gen, const unsigned char *pixels)
{
    if (!x || slot >= x->slotCount || x->gpu_producer)
        return M7G_T3_XS_UPLOAD;
    if (x->retired) {
        printf("T3_FAIL swapchain-retired (refusing further commits)\n");
        return M7G_T3_XS_RETIRED;
    }
    struct t1_slot *s = &x->slot[slot];
    VkResult res;

    if (s->usedOnce) {
        x->c.reuse_waits++;
        res = vkWaitForFences(x->device, 1, &s->fence, VK_TRUE, M7G_T3_FENCE_WAIT_NS);
        if (res != VK_SUCCESS) {
            printf("T3_FAIL reuse-wait slot=%u gen=%u res=%d\n", slot, gen, (int)res);
            return M7G_T3_XS_REUSE;
        }
    }
#ifdef M7G_TEST_HOOKS
    if (fail_at("upload")) return M7G_T3_XS_UPLOAD;
    if (fail_at("reuse"))  return M7G_T3_XS_REUSE;
#endif
    vkResetFences(x->device, 1, &s->fence);
    vkResetCommandBuffer(s->cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s->cmd, &bi) != VK_SUCCESS) {
        printf("T3_FAIL begin-cmd slot=%u\n", slot);
        return M7G_T3_XS_UPLOAD;
    }

    /* staging: host writes -> transfer reads (the only CPU-write target) */
    VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    hb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = s->staging;
    hb.offset = 0;
    hb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &hb, 0, NULL);

    /* reuse: acquire the shared buffer back from EXTERNAL */
    if (s->usedOnce) {
        VkBufferMemoryBarrier acq = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        acq.srcAccessMask = 0;
        acq.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acq.dstQueueFamilyIndex = x->qfam;
        acq.buffer = s->buffer;
        acq.offset = 0;
        acq.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &acq, 0, NULL);
    }

    memcpy(s->stagingMap, pixels, x->bytes);

    VkBufferCopy region = { 0, 0, x->bytes };
    vkCmdCopyBuffer(s->cmd, s->staging, s->buffer, 1, &region);

    /* release producer -> EXTERNAL so the compositor can consume the generation */
    VkBufferMemoryBarrier rel = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    rel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    rel.dstAccessMask = 0;
    rel.srcQueueFamilyIndex = x->qfam;
    rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    rel.buffer = s->buffer;
    rel.offset = 0;
    rel.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 1, &rel, 0, NULL);

    if (vkEndCommandBuffer(s->cmd) != VK_SUCCESS) {
        printf("T3_FAIL end-cmd slot=%u\n", slot);
        return M7G_T3_XS_UPLOAD;
    }

    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    if (s->usedOnce) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s->returned;
        si.pWaitDstStageMask = &stage;
    }
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s->cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s->ready;
    res = vkQueueSubmit(x->queue, 1, &si, s->fence);
    if (res != VK_SUCCESS) {
        printf("T3_FAIL submit slot=%u gen=%u res=%d\n", slot, gen, (int)res);
        return M7G_T3_XS_UPLOAD;
    }
    s->usedOnce = true;

    wl_surface_attach(x->surface, s->wlBuffer, 0, 0);
    wl_surface_damage_buffer(x->surface, 0, 0, (int32_t)x->w, (int32_t)x->h);
    wl_surface_commit(x->surface);
    if (wl_display_flush(x->display) < 0) {
        printf("T3_FAIL flush slot=%u gen=%u\n", slot, gen);
        return M7G_T3_XS_COMMIT;
    }
    if (wl_display_get_error(x->display) != 0) {
        printf("T3_FAIL protocol-error slot=%u gen=%u err=%d\n", slot, gen, wl_display_get_error(x->display));
        return M7G_T3_XS_COMMIT;
    }
    x->c.submitted++;
    x->c.commits++;
    printf("T3_COMMIT gen=%u slot=%u xid=0x%x\n", gen, slot, x->xid);
    return 0;
}

int m7g_t3_xport_service(m7g_t3_xport *x, int timeout_ms)
{
    if (!x || !x->display)
        return 0;
    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(x->display);
    pfd.events = POLLIN;
    pfd.revents = 0;
    while (wl_display_prepare_read(x->display) != 0) {
        if (wl_display_dispatch_pending(x->display) < 0) {
            printf("T3_FAIL service-dispatch err=%d\n", wl_display_get_error(x->display));
            return M7G_T3_XS_DRAIN;
        }
    }
    if (wl_display_flush(x->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(x->display);
        printf("T3_FAIL service-flush errno=%d\n", errno);
        return M7G_T3_XS_DRAIN;
    }
    int pr = poll(&pfd, 1, timeout_ms);
    {
        int verdict = m7g_t3_poll_verdict(pr, pfd.revents, errno);
        if (verdict != 0) {
            wl_display_cancel_read(x->display);
            printf("T3_FAIL service-poll pr=%d revents=0x%x errno=%d\n",
                   pr, (unsigned)pfd.revents, errno);
            return M7G_T3_XS_DRAIN;
        }
    }
    if (pr > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(x->display) < 0) {
            printf("T3_FAIL service-read err=%d\n", wl_display_get_error(x->display));
            return M7G_T3_XS_DRAIN;
        }
    } else {
        wl_display_cancel_read(x->display);
    }
    if (wl_display_dispatch_pending(x->display) < 0) {
        printf("T3_FAIL service-pending err=%d\n", wl_display_get_error(x->display));
        return M7G_T3_XS_DRAIN;
    }
    return 0;
}

int m7g_t3_xport_drain(m7g_t3_xport *x)
{
    if (!x || !x->display)
        return 0;
    if (wl_display_flush(x->display) < 0 && errno != EAGAIN) {
        printf("T3_FAIL drain-flush errno=%d\n", errno);
        return M7G_T3_XS_DRAIN;
    }
    if (wl_display_roundtrip(x->display) < 0 ||
        wl_display_get_error(x->display) != 0) {
        printf("T3_FAIL drain-roundtrip err=%d\n", wl_display_get_error(x->display));
        return M7G_T3_XS_DRAIN;
    }
    return 0;
}

int m7g_t3_xport_retired(const m7g_t3_xport *x)
{
    return x ? x->retired : 0;
}

void m7g_t3_xport_get_counters(const m7g_t3_xport *x, m7g_t3_xport_counters *c)
{
    *c = x->c;
}

int m7g_t3_xport_destroy(m7g_t3_xport *x)
{
    if (!x)
        return 0;
    if (x->gpu_producer) {
        for(unsigned i=0;i<x->slotCount;i++) if(x->slot[i].usedOnce) {
            m9w_log("GPU_FAIL stage=xport-teardown-not-drained slot=%u",i);
            return M7G_T3_XS_TEARDOWN;
        }
    }
    /* Fail-closed teardown: only free Vulkan/Wayland resources when every
     * in-flight submit is provably complete within a bounded wait.  If any
     * fence wait does not succeed (e.g. VK_TIMEOUT while a submit still waits
     * on `returned`), NOTHING is destroyed and the resources are left to
     * process exit; the caller keeps its original failure rc. */
    if (x->device) {
        for (unsigned i = 0; i < x->slotCount; i++) {
            struct t1_slot *s = &x->slot[i];
            if (!s->fence || !s->usedOnce)
                continue;
            VkResult r = vkWaitForFences(x->device, 1, &s->fence, VK_TRUE, M7G_T3_FENCE_WAIT_NS);
            if (r != VK_SUCCESS) {
                printf("T3_FAIL teardown-inflight slot=%u res=%d\n", i, (int)r);
                printf("T3_TEARDOWN_UNSAFE resources-left-to-process-exit=1\n");
                return M7G_T3_XS_TEARDOWN;
            }
        }
    }
    if (x->display) {
        for (unsigned i = 0; i < x->slotCount; i++) {
            struct t1_slot *s = &x->slot[i];
            if (s->wlBuffer) { wl_buffer_destroy(s->wlBuffer); s->wlBuffer = NULL; }
        }
        if (x->swapchain) { gamescope_swapchain_destroy(x->swapchain); x->swapchain = NULL; }
        if (x->surface) { wl_surface_destroy(x->surface); x->surface = NULL; }
        if (x->manager) { tegra_l4t_buffer_manager_destroy(x->manager); x->manager = NULL; }
        if (x->factory) { gamescope_swapchain_factory_v2_destroy(x->factory); x->factory = NULL; }
        if (x->compositor) { wl_compositor_destroy(x->compositor); x->compositor = NULL; }
        if (x->registry) { wl_registry_destroy(x->registry); x->registry = NULL; }
    }
    if (x->device) {
        for (unsigned i = 0; i < x->slotCount; i++) {
            struct t1_slot *s = &x->slot[i];
            if (s->stagingMap) { vkUnmapMemory(x->device, s->stagingMem); s->stagingMap = NULL; }
            if (s->staging) { vkDestroyBuffer(x->device, s->staging, NULL); s->staging = VK_NULL_HANDLE; }
            if (s->stagingMem) { vkFreeMemory(x->device, s->stagingMem, NULL); s->stagingMem = VK_NULL_HANDLE; }
            if (s->ready) { vkDestroySemaphore(x->device, s->ready, NULL); s->ready = VK_NULL_HANDLE; }
            if (s->returned) { vkDestroySemaphore(x->device, s->returned, NULL); s->returned = VK_NULL_HANDLE; }
            if (s->buffer) { vkDestroyBuffer(x->device, s->buffer, NULL); s->buffer = VK_NULL_HANDLE; }
            if (s->bufferMem) { vkFreeMemory(x->device, s->bufferMem, NULL); s->bufferMem = VK_NULL_HANDLE; }
            if (s->fence) { vkDestroyFence(x->device, s->fence, NULL); s->fence = VK_NULL_HANDLE; }
        }
        if (x->pool) { vkDestroyCommandPool(x->device, x->pool, NULL); x->pool = VK_NULL_HANDLE; }
        vkDestroyDevice(x->device, NULL);
        x->device = VK_NULL_HANDLE;
    }
    if (x->instance) { vkDestroyInstance(x->instance, NULL); x->instance = VK_NULL_HANDLE; }
    if (x->display) { wl_display_disconnect(x->display); x->display = NULL; }
    free(x);
    return 0;
}

/* External GL producer API. No Vulkan queue consumes returned or signals ready. */
int m9w_xport_export(m7g_t3_xport *x,unsigned i,struct m9w_slot_export *e) {
 if(!x||!x->gpu_producer||i>=x->slotCount)return 93;
 memset(e,0,sizeof(*e));e->memory_fd=e->ready_fd=e->returned_fd=-1;
 struct t1_slot *s=&x->slot[i];
 VkMemoryGetFdInfoKHR mi={.sType=VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,.memory=s->bufferMem,.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
 VkSemaphoreGetFdInfoKHR si={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
 if(x->getMemoryFd(x->device,&mi,&e->memory_fd)!=VK_SUCCESS)goto fail;
 si.semaphore=s->ready;if(x->getSemaphoreFd(x->device,&si,&e->ready_fd)!=VK_SUCCESS)goto fail;
 si.semaphore=s->returned;if(x->getSemaphoreFd(x->device,&si,&e->returned_fd)!=VK_SUCCESS)goto fail;
 e->bytes=x->bytes;e->allocation_size=s->allocSize;e->dedicated=s->dedicated;
 memcpy(e->device_uuid,x->deviceUuid,16);memcpy(e->driver_uuid,x->driverUuid,16);return 0;
fail:
 if(e->memory_fd>=0)close(e->memory_fd);
 if(e->ready_fd>=0)close(e->ready_fd);
 if(e->returned_fd>=0)close(e->returned_fd);
 return 93;
}
int m9w_xport_commit(m7g_t3_xport *x,unsigned i,unsigned gen) {
 if(!x||!x->gpu_producer||i>=x->slotCount||x->retired)return 93;
 struct t1_slot *s=&x->slot[i];
 if(x->m10k_gate && m10k_release_begin(&s->release)){
  m9w_log("GPU_FAIL stage=release-commit-pending xid=0x%x slot=%u gen=%u",x->xid,i,gen);return 25;
 }
 s->usedOnce=true;
 int trace=(x->w<500 && x->h>400) || gen<16 || gen%120==0;
 if(trace)m9w_log("M10K_WL_COMMIT xid=0x%x slot=%u gen=%u buffer=%u surface=%u edge=BEGIN",x->xid,i,gen,wl_proxy_get_id((struct wl_proxy*)s->wlBuffer),wl_proxy_get_id((struct wl_proxy*)x->surface));
 gamescope_swapchain_set_present_time(x->swapchain,gen+1,0,0);
 wl_surface_attach(x->surface,s->wlBuffer,0,0);wl_surface_damage_buffer(x->surface,0,0,x->w,x->h);wl_surface_commit(x->surface);
 int flushed=wl_display_flush(x->display),err=wl_display_get_error(x->display);
 if(trace)m9w_log("M10K_WL_COMMIT xid=0x%x slot=%u gen=%u edge=END flush=%d err=%d",x->xid,i,gen,flushed,err);
 if(flushed<0||err)return 93;
 x->c.submitted++;x->c.commits++;
 if(gen<8||gen%120==0)m9w_log("GPU_COMMIT gen=%u slot=%u xid=0x%x",gen,i,x->xid);
 return 0;
}
int m9w_xport_idle(m7g_t3_xport *x,unsigned i) {
 if(!x||!x->gpu_producer||i>=x->slotCount)return 93;
 x->slot[i].usedOnce=false;return 0;
}
int m9w_xport_returned_fd(m7g_t3_xport *x,unsigned i) {
 if(!x||!x->gpu_producer||i>=x->slotCount)return -1;
 VkSemaphoreGetFdInfoKHR info={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,.semaphore=x->slot[i].returned};
 int fd=-1;return x->getSemaphoreFd(x->device,&info,&fd)==VK_SUCCESS?fd:-1;
}

int m10k_xport_reassert(m7g_t3_xport *x){
 if(!x||!x->gpu_producer||x->retired)return 93;
 gamescope_swapchain_override_window_content(x->swapchain,x->server_id,x->xid);
 int flushed=wl_display_flush(x->display),err=wl_display_get_error(x->display);
 if((x->w<500 && x->h>400)||x->c.commits<8||x->c.commits%120==0)
  m9w_log("M10K_REASSERT xid=0x%x before_gen=%u surface=%u flush=%d err=%d",x->xid,x->c.commits,wl_proxy_get_id((struct wl_proxy*)x->surface),flushed,err);
 return flushed<0||err?93:0;
}
static uint64_t m10k_now(void *unused) {
 (void)unused; struct timespec ts;
 if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0) return UINT64_MAX;
 return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
}
static int m10k_pump(void *opaque,int ms) {
 struct m7g_t3_xport *x=opaque;
 int rc=m7g_t3_xport_service(x,ms);
 return rc ? rc : (x->retired ? M7G_T3_XS_RETIRED : 0);
}
int m10k_xport_release_gate(m7g_t3_xport *x,unsigned i) {
 if(!x || i>=x->slotCount) return M7G_T3_XS_REUSE;
 /* Set before frame 1 (even though it has no outstanding release). */
 if(!x->c.commits) {
  const char *v=getenv("M10K_RETURNED_RELEASE_GATE");
  x->m10k_gate=v && strcmp(v,"1")==0;
  m9w_log("M10K_VARIANT returned-release-v1 gate=%d xid=0x%x",x->m10k_gate,x->xid);
 }
 if(!x->m10k_gate) return 0;
 struct m10k_release_state *r=&x->slot[i].release;
 if(!r->pending) return 0;
 uint64_t begin=m10k_now(NULL);
 if(begin==UINT64_MAX) return M7G_T3_XS_REUSE;
 int rc=m10k_release_wait(r,begin+2000000000ull,m10k_now,m10k_pump,x);
 uint64_t end=m10k_now(NULL);
 if(rc || end-begin>50000000ull || r->serial<=8 || r->serial%120==0)
  m9w_log("M10K_RELEASE_WAIT xid=0x%x slot=%u serial=%llu rc=%d elapsed_ns=%llu pending=%u",x->xid,i,(unsigned long long)r->serial,rc,(unsigned long long)(end-begin),r->pending);
 if(rc)m9w_log("GPU_FAIL stage=release-before-returned xid=0x%x slot=%u serial=%llu rc=%d",x->xid,i,(unsigned long long)r->serial,rc);
 return rc;
}

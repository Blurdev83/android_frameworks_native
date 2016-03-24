/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBVULKAN_DRIVER_H
#define LIBVULKAN_DRIVER_H 1

#include <inttypes.h>
#include <bitset>
#include <type_traits>
#include <log/log.h>

#include <vulkan/vulkan.h>
#include <hardware/hwvulkan.h>

#include "api_gen.h"
#include "driver_gen.h"

namespace vulkan {

// This is here so that we can embed api::{Instance,Device}Data in
// driver::{Instance,Device}Data to avoid pointer chasing.  They are
// considered opaque to the driver layer.
namespace api {

struct InstanceData {
    InstanceDispatchTable dispatch;

    // for VkPhysicalDevice->VkInstance mapping
    VkInstance instance;

    // LayerChain::ActiveLayer array
    void* layers;
    uint32_t layer_count;

    // debug.vulkan.enable_callback
    PFN_vkDestroyDebugReportCallbackEXT destroy_debug_callback;
    VkDebugReportCallbackEXT debug_callback;
};

struct DeviceData {
    DeviceDispatchTable dispatch;

    // LayerChain::ActiveLayer array
    void* layers;
    uint32_t layer_count;
};

}  // namespace api

namespace driver {

struct InstanceData {
    InstanceData(const VkAllocationCallbacks& alloc)
        : opaque_api_data(),
          allocator(alloc),
          driver(),
          get_device_proc_addr(nullptr) {
        hook_extensions.set(ProcHook::EXTENSION_CORE);
        hal_extensions.set(ProcHook::EXTENSION_CORE);
    }

    api::InstanceData opaque_api_data;

    const VkAllocationCallbacks allocator;

    std::bitset<ProcHook::EXTENSION_COUNT> hook_extensions;
    std::bitset<ProcHook::EXTENSION_COUNT> hal_extensions;

    InstanceDriverTable driver;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};

struct DeviceData {
    DeviceData(const VkAllocationCallbacks& alloc)
        : opaque_api_data(), allocator(alloc), driver() {
        hook_extensions.set(ProcHook::EXTENSION_CORE);
        hal_extensions.set(ProcHook::EXTENSION_CORE);
    }

    api::DeviceData opaque_api_data;

    const VkAllocationCallbacks allocator;

    std::bitset<ProcHook::EXTENSION_COUNT> hook_extensions;
    std::bitset<ProcHook::EXTENSION_COUNT> hal_extensions;

    DeviceDriverTable driver;
};

bool Debuggable();
bool OpenHAL();
const VkAllocationCallbacks& GetDefaultAllocator();

// clang-format off
VKAPI_ATTR PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* pName);
VKAPI_ATTR VkResult EnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties);

VKAPI_ATTR VkResult CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
VKAPI_ATTR void DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR void GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
VKAPI_ATTR VkResult AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo, VkCommandBuffer* pCommandBuffers);
// clang-format on

template <typename DispatchableType>
void StaticAssertDispatchable(DispatchableType) {
    static_assert(std::is_same<DispatchableType, VkInstance>::value ||
                      std::is_same<DispatchableType, VkPhysicalDevice>::value ||
                      std::is_same<DispatchableType, VkDevice>::value ||
                      std::is_same<DispatchableType, VkQueue>::value ||
                      std::is_same<DispatchableType, VkCommandBuffer>::value,
                  "unrecognized dispatchable type");
}

template <typename DispatchableType>
bool SetDataInternal(DispatchableType dispatchable, const void* data) {
    StaticAssertDispatchable(dispatchable);

    hwvulkan_dispatch_t* dispatch =
        reinterpret_cast<hwvulkan_dispatch_t*>(dispatchable);
    // must be magic or already set
    if (dispatch->magic != HWVULKAN_DISPATCH_MAGIC && dispatch->vtbl != data) {
        ALOGE("invalid dispatchable object magic 0x%" PRIxPTR, dispatch->magic);
        return false;
    }

    dispatch->vtbl = data;

    return true;
}

template <typename DispatchableType>
void* GetDataInternal(DispatchableType dispatchable) {
    StaticAssertDispatchable(dispatchable);

    const hwvulkan_dispatch_t* dispatch =
        reinterpret_cast<const hwvulkan_dispatch_t*>(dispatchable);

    return const_cast<void*>(dispatch->vtbl);
}

inline bool SetData(VkInstance instance, const InstanceData& data) {
    return SetDataInternal(instance, &data);
}

inline bool SetData(VkPhysicalDevice physical_dev, const InstanceData& data) {
    return SetDataInternal(physical_dev, &data);
}

inline bool SetData(VkDevice dev, const DeviceData& data) {
    return SetDataInternal(dev, &data);
}

inline bool SetData(VkQueue queue, const DeviceData& data) {
    return SetDataInternal(queue, &data);
}

inline bool SetData(VkCommandBuffer cmd, const DeviceData& data) {
    return SetDataInternal(cmd, &data);
}

inline InstanceData& GetData(VkInstance instance) {
    return *reinterpret_cast<InstanceData*>(GetDataInternal(instance));
}

inline InstanceData& GetData(VkPhysicalDevice physical_dev) {
    return *reinterpret_cast<InstanceData*>(GetDataInternal(physical_dev));
}

inline DeviceData& GetData(VkDevice dev) {
    return *reinterpret_cast<DeviceData*>(GetDataInternal(dev));
}

inline DeviceData& GetData(VkQueue queue) {
    return *reinterpret_cast<DeviceData*>(GetDataInternal(queue));
}

inline DeviceData& GetData(VkCommandBuffer cmd) {
    return *reinterpret_cast<DeviceData*>(GetDataInternal(cmd));
}

}  // namespace driver
}  // namespace vulkan

#endif  // LIBVULKAN_DRIVER_H

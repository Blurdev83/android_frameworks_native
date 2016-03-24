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

#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <new>
#include <malloc.h>
#include <sys/prctl.h>

#include "driver.h"
#include "loader.h"

// #define ENABLE_ALLOC_CALLSTACKS 1
#if ENABLE_ALLOC_CALLSTACKS
#include <utils/CallStack.h>
#define ALOGD_CALLSTACK(...)                             \
    do {                                                 \
        ALOGD(__VA_ARGS__);                              \
        android::CallStack callstack;                    \
        callstack.update();                              \
        callstack.log(LOG_TAG, ANDROID_LOG_DEBUG, "  "); \
    } while (false)
#else
#define ALOGD_CALLSTACK(...) \
    do {                     \
    } while (false)
#endif

namespace vulkan {
namespace driver {

namespace {

class CreateInfoWrapper {
   public:
    CreateInfoWrapper(VkPhysicalDevice physical_dev,
                      const VkDeviceCreateInfo& create_info,
                      const VkAllocationCallbacks& allocator);
    ~CreateInfoWrapper();

    VkResult validate();

    const std::bitset<ProcHook::EXTENSION_COUNT>& get_hook_extensions() const;
    const std::bitset<ProcHook::EXTENSION_COUNT>& get_hal_extensions() const;

    explicit operator const VkDeviceCreateInfo*() const;

   private:
    struct ExtensionFilter {
        VkExtensionProperties* exts;
        uint32_t ext_count;

        const char** names;
        uint32_t name_count;
    };

    VkResult sanitize_pnext();

    VkResult sanitize_layers();
    VkResult sanitize_extensions();

    VkResult query_extension_count(uint32_t& count) const;
    VkResult enumerate_extensions(uint32_t& count,
                                  VkExtensionProperties* props) const;
    VkResult init_extension_filter();
    void filter_extension(const char* name);

    const bool is_instance_;
    const VkAllocationCallbacks& allocator_;

    union {
        hwvulkan_device_t* hw_dev_;
        VkPhysicalDevice physical_dev_;
    };

    union {
        VkInstanceCreateInfo instance_info_;
        VkDeviceCreateInfo dev_info_;
    };

    ExtensionFilter extension_filter_;

    std::bitset<ProcHook::EXTENSION_COUNT> hook_extensions_;
    std::bitset<ProcHook::EXTENSION_COUNT> hal_extensions_;
};

CreateInfoWrapper::CreateInfoWrapper(VkPhysicalDevice physical_dev,
                                     const VkDeviceCreateInfo& create_info,
                                     const VkAllocationCallbacks& allocator)
    : is_instance_(false),
      allocator_(allocator),
      physical_dev_(physical_dev),
      dev_info_(create_info),
      extension_filter_() {
    hook_extensions_.set(ProcHook::EXTENSION_CORE);
    hal_extensions_.set(ProcHook::EXTENSION_CORE);
}

CreateInfoWrapper::~CreateInfoWrapper() {
    allocator_.pfnFree(allocator_.pUserData, extension_filter_.exts);
    allocator_.pfnFree(allocator_.pUserData, extension_filter_.names);
}

VkResult CreateInfoWrapper::validate() {
    VkResult result = sanitize_pnext();
    if (result == VK_SUCCESS)
        result = sanitize_layers();
    if (result == VK_SUCCESS)
        result = sanitize_extensions();

    return result;
}

const std::bitset<ProcHook::EXTENSION_COUNT>&
CreateInfoWrapper::get_hook_extensions() const {
    return hook_extensions_;
}

const std::bitset<ProcHook::EXTENSION_COUNT>&
CreateInfoWrapper::get_hal_extensions() const {
    return hal_extensions_;
}

CreateInfoWrapper::operator const VkDeviceCreateInfo*() const {
    return &dev_info_;
}

VkResult CreateInfoWrapper::sanitize_pnext() {
    const struct StructHeader {
        VkStructureType type;
        const void* next;
    } * header;

    if (is_instance_) {
        header = reinterpret_cast<const StructHeader*>(instance_info_.pNext);

        // skip leading VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFOs
        while (header &&
               header->type == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            header = reinterpret_cast<const StructHeader*>(header->next);

        instance_info_.pNext = header;
    } else {
        header = reinterpret_cast<const StructHeader*>(dev_info_.pNext);

        // skip leading VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFOs
        while (header &&
               header->type == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            header = reinterpret_cast<const StructHeader*>(header->next);

        dev_info_.pNext = header;
    }

    return VK_SUCCESS;
}

VkResult CreateInfoWrapper::sanitize_layers() {
    auto& layer_names = (is_instance_) ? instance_info_.ppEnabledLayerNames
                                       : dev_info_.ppEnabledLayerNames;
    auto& layer_count = (is_instance_) ? instance_info_.enabledLayerCount
                                       : dev_info_.enabledLayerCount;

    // remove all layers
    layer_names = nullptr;
    layer_count = 0;

    return VK_SUCCESS;
}

VkResult CreateInfoWrapper::sanitize_extensions() {
    auto& ext_names = (is_instance_) ? instance_info_.ppEnabledExtensionNames
                                     : dev_info_.ppEnabledExtensionNames;
    auto& ext_count = (is_instance_) ? instance_info_.enabledExtensionCount
                                     : dev_info_.enabledExtensionCount;
    if (!ext_count)
        return VK_SUCCESS;

    VkResult result = init_extension_filter();
    if (result != VK_SUCCESS)
        return result;

    for (uint32_t i = 0; i < ext_count; i++)
        filter_extension(ext_names[i]);

    ext_names = extension_filter_.names;
    ext_count = extension_filter_.name_count;

    return VK_SUCCESS;
}

VkResult CreateInfoWrapper::query_extension_count(uint32_t& count) const {
    if (is_instance_) {
        return hw_dev_->EnumerateInstanceExtensionProperties(nullptr, &count,
                                                             nullptr);
    } else {
        const auto& driver = GetData(physical_dev_).driver;
        return driver.EnumerateDeviceExtensionProperties(physical_dev_, nullptr,
                                                         &count, nullptr);
    }
}

VkResult CreateInfoWrapper::enumerate_extensions(
    uint32_t& count,
    VkExtensionProperties* props) const {
    if (is_instance_) {
        return hw_dev_->EnumerateInstanceExtensionProperties(nullptr, &count,
                                                             props);
    } else {
        const auto& driver = GetData(physical_dev_).driver;
        return driver.EnumerateDeviceExtensionProperties(physical_dev_, nullptr,
                                                         &count, props);
    }
}

VkResult CreateInfoWrapper::init_extension_filter() {
    // query extension count
    uint32_t count;
    VkResult result = query_extension_count(count);
    if (result != VK_SUCCESS || count == 0)
        return result;

    auto& filter = extension_filter_;
    filter.exts =
        reinterpret_cast<VkExtensionProperties*>(allocator_.pfnAllocation(
            allocator_.pUserData, sizeof(VkExtensionProperties) * count,
            alignof(VkExtensionProperties),
            VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
    if (!filter.exts)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    // enumerate extensions
    result = enumerate_extensions(count, filter.exts);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;

    if (!count)
        return VK_SUCCESS;

    filter.ext_count = count;

    // allocate name array
    uint32_t enabled_ext_count = (is_instance_)
                                     ? instance_info_.enabledExtensionCount
                                     : dev_info_.enabledExtensionCount;
    count = std::min(filter.ext_count, enabled_ext_count);
    filter.names = reinterpret_cast<const char**>(allocator_.pfnAllocation(
        allocator_.pUserData, sizeof(const char*) * count, alignof(const char*),
        VK_SYSTEM_ALLOCATION_SCOPE_COMMAND));
    if (!filter.names)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    return VK_SUCCESS;
}

void CreateInfoWrapper::filter_extension(const char* name) {
    auto& filter = extension_filter_;

    ProcHook::Extension ext_bit = GetProcHookExtension(name);
    if (is_instance_) {
        switch (ext_bit) {
            case ProcHook::KHR_android_surface:
            case ProcHook::KHR_surface:
                hook_extensions_.set(ext_bit);
                // return now as these extensions do not require HAL support
                return;
            case ProcHook::EXT_debug_report:
                // both we and HAL can take part in
                hook_extensions_.set(ext_bit);
                break;
            case ProcHook::EXTENSION_UNKNOWN:
                // HAL's extensions
                break;
            default:
                ALOGW("Ignored invalid instance extension %s", name);
                return;
        }
    } else {
        switch (ext_bit) {
            case ProcHook::KHR_swapchain:
                // map VK_KHR_swapchain to VK_ANDROID_native_buffer
                name = VK_ANDROID_NATIVE_BUFFER_EXTENSION_NAME;
                ext_bit = ProcHook::ANDROID_native_buffer;
                break;
            case ProcHook::EXTENSION_UNKNOWN:
                // HAL's extensions
                break;
            default:
                ALOGW("Ignored invalid device extension %s", name);
                return;
        }
    }

    for (uint32_t i = 0; i < filter.ext_count; i++) {
        const VkExtensionProperties& props = filter.exts[i];
        // ignore unknown extensions
        if (strcmp(name, props.extensionName) != 0)
            continue;

        if (ext_bit == ProcHook::ANDROID_native_buffer)
            hook_extensions_.set(ProcHook::KHR_swapchain);

        filter.names[filter.name_count++] = name;
        hal_extensions_.set(ext_bit);

        break;
    }
}

hwvulkan_device_t* g_hwdevice = nullptr;

VKAPI_ATTR void* DefaultAllocate(void*,
                                 size_t size,
                                 size_t alignment,
                                 VkSystemAllocationScope) {
    void* ptr = nullptr;
    // Vulkan requires 'alignment' to be a power of two, but posix_memalign
    // additionally requires that it be at least sizeof(void*).
    int ret = posix_memalign(&ptr, std::max(alignment, sizeof(void*)), size);
    ALOGD_CALLSTACK("Allocate: size=%zu align=%zu => (%d) %p", size, alignment,
                    ret, ptr);
    return ret == 0 ? ptr : nullptr;
}

VKAPI_ATTR void* DefaultReallocate(void*,
                                   void* ptr,
                                   size_t size,
                                   size_t alignment,
                                   VkSystemAllocationScope) {
    if (size == 0) {
        free(ptr);
        return nullptr;
    }

    // TODO(jessehall): Right now we never shrink allocations; if the new
    // request is smaller than the existing chunk, we just continue using it.
    // Right now the loader never reallocs, so this doesn't matter. If that
    // changes, or if this code is copied into some other project, this should
    // probably have a heuristic to allocate-copy-free when doing so will save
    // "enough" space.
    size_t old_size = ptr ? malloc_usable_size(ptr) : 0;
    if (size <= old_size)
        return ptr;

    void* new_ptr = nullptr;
    if (posix_memalign(&new_ptr, std::max(alignment, sizeof(void*)), size) != 0)
        return nullptr;
    if (ptr) {
        memcpy(new_ptr, ptr, std::min(old_size, size));
        free(ptr);
    }
    return new_ptr;
}

VKAPI_ATTR void DefaultFree(void*, void* ptr) {
    ALOGD_CALLSTACK("Free: %p", ptr);
    free(ptr);
}

DeviceData* AllocateDeviceData(const VkAllocationCallbacks& allocator) {
    void* data_mem = allocator.pfnAllocation(
        allocator.pUserData, sizeof(DeviceData), alignof(DeviceData),
        VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    if (!data_mem)
        return nullptr;

    return new (data_mem) DeviceData(allocator);
}

void FreeDeviceData(DeviceData* data, const VkAllocationCallbacks& allocator) {
    data->~DeviceData();
    allocator.pfnFree(allocator.pUserData, data);
}

}  // anonymous namespace

bool Debuggable() {
    return (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) >= 0);
}

bool OpenHAL() {
    if (g_hwdevice)
        return true;

    const hwvulkan_module_t* module;
    int result =
        hw_get_module("vulkan", reinterpret_cast<const hw_module_t**>(&module));
    if (result != 0) {
        ALOGE("failed to load vulkan hal: %s (%d)", strerror(-result), result);
        return false;
    }

    hwvulkan_device_t* device;
    result =
        module->common.methods->open(&module->common, HWVULKAN_DEVICE_0,
                                     reinterpret_cast<hw_device_t**>(&device));
    if (result != 0) {
        ALOGE("failed to open vulkan driver: %s (%d)", strerror(-result),
              result);
        return false;
    }

    if (!InitLoader(device)) {
        device->common.close(&device->common);
        return false;
    }

    g_hwdevice = device;

    return true;
}

const VkAllocationCallbacks& GetDefaultAllocator() {
    static const VkAllocationCallbacks kDefaultAllocCallbacks = {
        .pUserData = nullptr,
        .pfnAllocation = DefaultAllocate,
        .pfnReallocation = DefaultReallocate,
        .pfnFree = DefaultFree,
    };

    return kDefaultAllocCallbacks;
}

PFN_vkVoidFunction GetInstanceProcAddr(VkInstance instance, const char* pName) {
    const ProcHook* hook = GetProcHook(pName);
    if (!hook)
        return g_hwdevice->GetInstanceProcAddr(instance, pName);

    if (!instance) {
        if (hook->type == ProcHook::GLOBAL)
            return hook->proc;

        ALOGE(
            "Invalid use of vkGetInstanceProcAddr to query %s without an "
            "instance",
            pName);

        // Some naughty layers expect
        //
        //   vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
        //
        // to work.
        return (strcmp(pName, "vkCreateDevice") == 0) ? hook->proc : nullptr;
    }

    PFN_vkVoidFunction proc;

    switch (hook->type) {
        case ProcHook::INSTANCE:
            proc = (GetData(instance).hook_extensions[hook->extension])
                       ? hook->proc
                       : hook->disabled_proc;
            break;
        case ProcHook::DEVICE:
            proc = (hook->extension == ProcHook::EXTENSION_CORE)
                       ? hook->proc
                       : hook->checked_proc;
            break;
        default:
            ALOGE(
                "Invalid use of vkGetInstanceProcAddr to query %s with an "
                "instance",
                pName);
            proc = nullptr;
            break;
    }

    return proc;
}

PFN_vkVoidFunction GetDeviceProcAddr(VkDevice device, const char* pName) {
    const ProcHook* hook = GetProcHook(pName);
    if (!hook)
        return GetData(device).driver.GetDeviceProcAddr(device, pName);

    if (hook->type != ProcHook::DEVICE) {
        ALOGE("Invalid use of vkGetDeviceProcAddr to query %s", pName);
        return nullptr;
    }

    return (GetData(device).hook_extensions[hook->extension])
               ? hook->proc
               : hook->disabled_proc;
}

VkResult CreateDevice(VkPhysicalDevice physicalDevice,
                      const VkDeviceCreateInfo* pCreateInfo,
                      const VkAllocationCallbacks* pAllocator,
                      VkDevice* pDevice) {
    const InstanceData& instance_data = GetData(physicalDevice);
    const VkAllocationCallbacks& data_allocator =
        (pAllocator) ? *pAllocator : instance_data.allocator;

    CreateInfoWrapper wrapper(physicalDevice, *pCreateInfo, data_allocator);
    VkResult result = wrapper.validate();
    if (result != VK_SUCCESS)
        return result;

    DeviceData* data = AllocateDeviceData(data_allocator);
    if (!data)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    data->hook_extensions |= wrapper.get_hook_extensions();
    data->hal_extensions |= wrapper.get_hal_extensions();

    // call into the driver
    VkDevice dev;
    result = instance_data.driver.CreateDevice(
        physicalDevice, static_cast<const VkDeviceCreateInfo*>(wrapper),
        pAllocator, &dev);
    if (result != VK_SUCCESS) {
        FreeDeviceData(data, data_allocator);
        return result;
    }

    // initialize DeviceDriverTable
    if (!SetData(dev, *data) ||
        !InitDriverTable(dev, instance_data.get_device_proc_addr)) {
        data->driver.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(
            instance_data.get_device_proc_addr(dev, "vkDestroyDevice"));
        if (data->driver.DestroyDevice)
            data->driver.DestroyDevice(dev, pAllocator);

        FreeDeviceData(data, data_allocator);

        return VK_ERROR_INCOMPATIBLE_DRIVER;
    }

    *pDevice = dev;

    return VK_SUCCESS;
}

void DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    DeviceData& data = GetData(device);
    data.driver.DestroyDevice(device, pAllocator);

    VkAllocationCallbacks local_allocator;
    if (!pAllocator) {
        local_allocator = data.allocator;
        pAllocator = &local_allocator;
    }

    FreeDeviceData(&data, *pAllocator);
}

void GetDeviceQueue(VkDevice device,
                    uint32_t queueFamilyIndex,
                    uint32_t queueIndex,
                    VkQueue* pQueue) {
    const auto& data = GetData(device);

    data.driver.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    SetData(*pQueue, data);
}

VKAPI_ATTR VkResult
AllocateCommandBuffers(VkDevice device,
                       const VkCommandBufferAllocateInfo* pAllocateInfo,
                       VkCommandBuffer* pCommandBuffers) {
    const auto& data = GetData(device);

    VkResult result = data.driver.AllocateCommandBuffers(device, pAllocateInfo,
                                                         pCommandBuffers);
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++)
            SetData(pCommandBuffers[i], data);
    }

    return result;
}

}  // namespace driver
}  // namespace vulkan

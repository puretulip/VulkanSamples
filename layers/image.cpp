/*
 * Vulkan
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>
#include <unordered_map>
#include <memory>
using namespace std;

#include "vk_loader_platform.h"
#include "vk_dispatch_table_helper.h"
#include "vk_struct_string_helper_cpp.h"
#include "vk_enum_validate_helper.h"
#include "image.h"
#include "vk_layer_config.h"
#include "vk_layer_extension_utils.h"
#include "vk_layer_table.h"
#include "vk_layer_data.h"
#include "vk_layer_extension_utils.h"
#include "vk_layer_utils.h"
#include "vk_layer_logging.h"

using namespace std;

struct layer_data {
    debug_report_data *report_data;
    std::vector<VkDbgMsgCallback> logging_callback;
    VkPhysicalDevice physicalDevice;
    unordered_map<uint64_t, IMAGE_STATE> imageMap;

    layer_data() :
        report_data(nullptr),
        physicalDevice(0)
    {};
};

static unordered_map<void*, layer_data*> layer_data_map;
static device_table_map   image_device_table_map;
static instance_table_map image_instance_table_map;

// "my device data"
debug_report_data *mdd(const void* object)
{
    dispatch_key key = get_dispatch_key(object);
    layer_data *data = get_my_data_ptr(key, layer_data_map);
#if DISPATCH_MAP_DEBUG
    fprintf(stderr, "MDD: map: %p, object: %p, key: %p, data: %p\n", &layer_data_map, object, key, data);
#endif
    return data->report_data;
}

// "my instance data"
debug_report_data *mid(VkInstance object)
{
    dispatch_key key = get_dispatch_key(object);
    layer_data *data = get_my_data_ptr(key, layer_data_map);
#if DISPATCH_MAP_DEBUG
    fprintf(stderr, "MID: map: %p, object: %p, key: %p, data: %p\n", &layer_data_map, object, key, data);
#endif
    return data->report_data;
}

static void InitImage(layer_data *data)
{
    VkDbgMsgCallback callback;
    uint32_t report_flags = getLayerOptionFlags("ImageReportFlags", 0);

    uint32_t debug_action = 0;
    getLayerOptionEnum("ImageDebugAction", (uint32_t *) &debug_action);
    if(debug_action & VK_DBG_LAYER_ACTION_LOG_MSG)
    {
        FILE *log_output = NULL;
        const char* option_str = getLayerOption("ImageLogFilename");
        log_output = getLayerLogOutput(option_str, "Image");
        layer_create_msg_callback(data->report_data, report_flags, log_callback, (void *) log_output, &callback);
        data->logging_callback.push_back(callback);
    }

    if (debug_action & VK_DBG_LAYER_ACTION_DEBUG_OUTPUT) {
        layer_create_msg_callback(data->report_data, report_flags, win32_debug_output_msg, NULL, &callback);
        data->logging_callback.push_back(callback);
    }
}

VK_LAYER_EXPORT VkResult VKAPI vkDbgCreateMsgCallback(
        VkInstance instance,
        VkFlags msgFlags,
        const PFN_vkDbgMsgCallback pfnMsgCallback,
        void* pUserData,
        VkDbgMsgCallback* pMsgCallback)
{
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(image_instance_table_map, instance);
    VkResult res =  pTable->DbgCreateMsgCallback(instance, msgFlags, pfnMsgCallback, pUserData, pMsgCallback);
    if (res == VK_SUCCESS) {
        layer_data *data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);

        res = layer_create_msg_callback(data->report_data, msgFlags, pfnMsgCallback, pUserData, pMsgCallback);
    }
    return res;
}

VK_LAYER_EXPORT VkResult VKAPI vkDbgDestroyMsgCallback(
        VkInstance instance,
        VkDbgMsgCallback msgCallback)
{
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(image_instance_table_map, instance);
    VkResult res =  pTable->DbgDestroyMsgCallback(instance, msgCallback);

    layer_data *data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);
    layer_destroy_msg_callback(data->report_data, msgCallback);

    return res;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, VkInstance* pInstance)
{
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(image_instance_table_map, *pInstance);
    VkResult result = pTable->CreateInstance(pCreateInfo, pInstance);

    if (result == VK_SUCCESS) {
        layer_data *data = get_my_data_ptr(get_dispatch_key(*pInstance), layer_data_map);
        data->report_data = debug_report_create_instance(pTable, *pInstance, pCreateInfo->extensionCount,
            pCreateInfo->ppEnabledExtensionNames);

        InitImage(data);
    }

    return result;
}

VK_LAYER_EXPORT void VKAPI vkDestroyInstance(VkInstance instance)
{
    // Grab the key before the instance is destroyed.
    dispatch_key key = get_dispatch_key(instance);
    VkLayerInstanceDispatchTable *pTable = get_dispatch_table(image_instance_table_map, instance);
    pTable->DestroyInstance(instance);

    // Clean up logging callback, if any
    layer_data *my_data = get_my_data_ptr(key, layer_data_map);
    while (my_data->logging_callback.size() > 0) {
        VkDbgMsgCallback callback = my_data->logging_callback.back();
        layer_destroy_msg_callback(my_data->report_data, callback);
        my_data->logging_callback.pop_back();
    }

    layer_debug_report_destroy_instance(mid(instance));
    layer_data_map.erase(pTable);

    image_instance_table_map.erase(key);
    assert(image_instance_table_map.size() == 0 && "Should not have any instance mappings hanging around");
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, VkDevice* pDevice)
{
    VkLayerDispatchTable *pTable = get_dispatch_table(image_device_table_map, *pDevice);
    VkResult result = pTable->CreateDevice(physicalDevice, pCreateInfo, pDevice);
    if(result == VK_SUCCESS)
    {
        layer_data *instance_data = get_my_data_ptr(get_dispatch_key(physicalDevice), layer_data_map);
        layer_data *device_data = get_my_data_ptr(get_dispatch_key(*pDevice), layer_data_map);
        device_data->report_data = layer_debug_report_create_device(instance_data->report_data, *pDevice);
        device_data->physicalDevice = physicalDevice;
    }

    return result;
}

VK_LAYER_EXPORT void VKAPI vkDestroyDevice(VkDevice device)
{
    layer_debug_report_destroy_device(device);

    dispatch_key key = get_dispatch_key(device);
#if DISPATCH_MAP_DEBUG
    fprintf(stderr, "Device: %p, key: %p\n", device, key);
#endif

    get_dispatch_table(image_device_table_map, device)->DestroyDevice(device);
    image_device_table_map.erase(key);
    assert(image_device_table_map.size() == 0 && "Should not have any instance mappings hanging around");
}

static const VkLayerProperties pc_global_layers[] = {
    {
        "Image",
        VK_API_VERSION,
        VK_MAKE_VERSION(0, 1, 0),
        "Validation layer: Image ParamChecker",
    }
};

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateInstanceExtensionProperties(
        const char *pLayerName,
        uint32_t *pCount,
        VkExtensionProperties* pProperties)
{
    // ParamChecker does not have any global extensions
    return util_GetExtensionProperties(0, NULL, pCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateInstanceLayerProperties(
        uint32_t *pCount,
        VkLayerProperties*    pProperties)
{
    return util_GetLayerProperties(ARRAY_SIZE(pc_global_layers),
                                   pc_global_layers,
                                   pCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateDeviceExtensionProperties(
        VkPhysicalDevice                            physicalDevice,
        const char*                                 pLayerName,
        uint32_t*                                   pCount,
        VkExtensionProperties*                      pProperties)
{
    // ParamChecker does not have any physical device extensions
    return util_GetExtensionProperties(0, NULL, pCount, pProperties);
}

VK_LAYER_EXPORT VkResult VKAPI vkEnumerateDeviceLayerProperties(
        VkPhysicalDevice                            physicalDevice,
        uint32_t*                                   pCount,
        VkLayerProperties*                          pProperties)
{
    // ParamChecker's physical device layers are the same as global
    return util_GetLayerProperties(ARRAY_SIZE(pc_global_layers), pc_global_layers,
                                   pCount, pProperties);
}

// Start of the Image layer proper

// Returns TRUE if a format is a depth-compatible format
bool is_depth_format(VkFormat format)
{
    bool result = VK_FALSE;
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D24_UNORM_X8:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            result = VK_TRUE;
            break;
        default:
            break;
    }
    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo, VkImage* pImage)
{
    VkBool32 skipCall = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    if(pCreateInfo->format != VK_FORMAT_UNDEFINED)
    {
        VkFormatProperties properties;
        get_dispatch_table(image_instance_table_map, device_data->physicalDevice)->GetPhysicalDeviceFormatProperties(
                device_data->physicalDevice, pCreateInfo->format, &properties);

        if((properties.linearTilingFeatures) == 0 && (properties.optimalTilingFeatures == 0))
        {
            char const str[] = "vkCreateImage parameter, VkFormat pCreateInfo->format, contains unsupported format";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_FORMAT_UNSUPPORTED, "IMAGE", str);
        }
    }
    if (skipCall)
        return VK_ERROR_VALIDATION_FAILED;

    VkResult result = get_dispatch_table(image_device_table_map, device)->CreateImage(device, pCreateInfo, pImage);

    if(result == VK_SUCCESS) {
        device_data->imageMap[pImage->handle] = IMAGE_STATE(pCreateInfo);
    }
    return result;
}

VK_LAYER_EXPORT void VKAPI vkDestroyImage(VkDevice device, VkImage image)
{
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    device_data->imageMap.erase(image.handle);
    get_dispatch_table(image_device_table_map, device)->DestroyImage(device, image);
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo, VkRenderPass* pRenderPass)
{
    VkBool32 skipCall = VK_FALSE;
    for(uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        if(pCreateInfo->pAttachments[i].format != VK_FORMAT_UNDEFINED)
        {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
            VkFormatProperties properties;
            get_dispatch_table(image_instance_table_map, device_data->physicalDevice)->GetPhysicalDeviceFormatProperties(
                    device_data->physicalDevice, pCreateInfo->pAttachments[i].format, &properties);

            if((properties.linearTilingFeatures) == 0 && (properties.optimalTilingFeatures == 0))
            {
                std::stringstream ss;
                ss << "vkCreateRenderPass parameter, VkFormat in pCreateInfo->pAttachments[" << i << "], contains unsupported format";
                skipCall |= log_msg(mdd(device), VK_DBG_REPORT_WARN_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_FORMAT_UNSUPPORTED, "IMAGE", ss.str().c_str());
            }
        }
    }

    for(uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        if(!validate_VkImageLayout(pCreateInfo->pAttachments[i].initialLayout) ||
           !validate_VkImageLayout(pCreateInfo->pAttachments[i].finalLayout))
        {
            std::stringstream ss;
            ss << "vkCreateRenderPass parameter, VkImageLayout in pCreateInfo->pAttachments[" << i << "], is unrecognized";
            skipCall |= log_msg(mdd(device), VK_DBG_REPORT_WARN_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_RENDERPASS_INVALID_ATTACHMENT, "IMAGE", ss.str().c_str());
        }
    }

    for(uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        if(!validate_VkAttachmentLoadOp(pCreateInfo->pAttachments[i].loadOp))
        {
            std::stringstream ss;
            ss << "vkCreateRenderPass parameter, VkAttachmentLoadOp in pCreateInfo->pAttachments[" << i << "], is unrecognized";
            skipCall |= log_msg(mdd(device), VK_DBG_REPORT_WARN_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_RENDERPASS_INVALID_ATTACHMENT, "IMAGE", ss.str().c_str());
        }
    }

    for(uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        if(!validate_VkAttachmentStoreOp(pCreateInfo->pAttachments[i].storeOp))
        {
            std::stringstream ss;
            ss << "vkCreateRenderPass parameter, VkAttachmentStoreOp in pCreateInfo->pAttachments[" << i << "], is unrecognized";
            skipCall |= log_msg(mdd(device), VK_DBG_REPORT_WARN_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_RENDERPASS_INVALID_ATTACHMENT, "IMAGE", ss.str().c_str());
        }
    }

    // Any depth buffers specified as attachments?
    bool depthFormatPresent = VK_FALSE;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
    {
        depthFormatPresent |= is_depth_format(pCreateInfo->pAttachments[i].format);
    }

    if (depthFormatPresent == VK_FALSE) {
        // No depth attachment is present, validate that subpasses set depthStencilAttachment to VK_ATTACHMENT_UNUSED;
        for (uint32_t i = 0; i < pCreateInfo->subpassCount; i++) {
            if (pCreateInfo->pSubpasses[i].depthStencilAttachment.attachment != VK_ATTACHMENT_UNUSED) {
                std::stringstream ss;
                ss << "vkCreateRenderPass has no depth/stencil attachment, yet subpass[" << i << "] has VkSubpassDescription::depthStencilAttachment value that is not VK_ATTACHMENT_UNUSED";
                skipCall |= log_msg(mdd(device), VK_DBG_REPORT_ERROR_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_RENDERPASS_INVALID_DS_ATTACHMENT, "IMAGE", ss.str().c_str());
            }
        }
    }
    if (skipCall)
        return VK_ERROR_VALIDATION_FAILED;

    VkResult result = get_dispatch_table(image_device_table_map, device)->CreateRenderPass(device, pCreateInfo, pRenderPass);

    return result;
}

VK_LAYER_EXPORT VkResult VKAPI vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo, VkImageView* pView)
{
    VkBool32 skipCall = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    auto imageEntry = device_data->imageMap.find(pCreateInfo->image.handle);
    if (imageEntry != device_data->imageMap.end()) {
        if (pCreateInfo->subresourceRange.baseMipLevel >= imageEntry->second.mipLevels) {
            std::stringstream ss;
            ss << "vkCreateImageView called with baseMipLevel " << pCreateInfo->subresourceRange.baseMipLevel
               << " for image " << pCreateInfo->image.handle << " that only has " << imageEntry->second.mipLevels << " mip levels.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_VIEW_CREATE_ERROR, "IMAGE", ss.str().c_str());
        }
        if (pCreateInfo->subresourceRange.baseArrayLayer >= imageEntry->second.arraySize) {
            std::stringstream ss;
            ss << "vkCreateImageView called with baseArrayLayer " << pCreateInfo->subresourceRange.baseArrayLayer << " for image "
               << pCreateInfo->image.handle << " that only has " << imageEntry->second.arraySize << " mip levels.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_VIEW_CREATE_ERROR, "IMAGE", ss.str().c_str());
        }
        if (!pCreateInfo->subresourceRange.numLevels) {
            std::stringstream ss;
            ss << "vkCreateImageView called with 0 in pCreateInfo->subresourceRange.mipLevels.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_VIEW_CREATE_ERROR, "IMAGE", ss.str().c_str());
        }
        if (!pCreateInfo->subresourceRange.numLayers) {
            std::stringstream ss;
            ss << "vkCreateImageView called with 0 in pCreateInfo->subresourceRange.arraySize.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, (VkDbgObjectType)0, 0, 0, IMAGE_VIEW_CREATE_ERROR, "IMAGE", ss.str().c_str());
        }

        // Validate correct image aspect bits for desired formats and format consistency
        VkFormat           imageFormat = imageEntry->second.format;
        VkFormat           ivciFormat  = pCreateInfo->format;
        VkImageAspectFlags aspectMask  = pCreateInfo->subresourceRange.aspectMask;

        if (vk_format_is_color(imageFormat)) {
            if ((aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != VK_IMAGE_ASPECT_COLOR_BIT) {
                std::stringstream ss;
                ss << "vkCreateImageView: Color image formats must have the VK_IMAGE_ASPECT_COLOR_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
            if ((aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != aspectMask) {
                std::stringstream ss;
                ss << "vkCreateImageView: Color image formats must have ONLY the VK_IMAGE_ASPECT_COLOR_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
            if (VK_FALSE == vk_format_is_color(ivciFormat)) {
                std::stringstream ss;
                ss << "vkCreateImageView: The image view's format can differ from the parent image's format, but both must be "
                   << "color formats.  ImageFormat is " << string_VkFormat(imageFormat) << " ImageViewFormat is " << string_VkFormat(ivciFormat);
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_FORMAT, "IMAGE", ss.str().c_str());
            }
            // TODO:  Uncompressed formats are compatible if they occupy they same number of bits per pixel.
            //        Compressed formats are compatible if the only difference between them is the numerical type of
            //        the uncompressed pixels (e.g. signed vs. unsigned, or sRGB vs. UNORM encoding).
        } else  if (vk_format_is_depth_and_stencil(imageFormat)) {
            if ((aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) !=
                              (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                std::stringstream ss;
                ss << "vkCreateImageView: Combination depth/stencil image formats must have both VK_IMAGE_ASPECT_DEPTH_BIT and VK_IMAGE_ASPECT_STENCIL_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
            if ((aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != aspectMask) {
                std::stringstream ss;
                ss << "vkCreateImageView: Combination depth/stencil image formats can have only the VK_IMAGE_ASPECT_DEPTH_BIT and VK_IMAGE_ASPECT_STENCIL_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
        } else  if (vk_format_is_depth_only(imageFormat)) {
            if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != VK_IMAGE_ASPECT_DEPTH_BIT) {
                std::stringstream ss;
                ss << "vkCreateImageView: Depth-only image formats must have the VK_IMAGE_ASPECT_DEPTH_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
            if ((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != aspectMask) {
                std::stringstream ss;
                ss << "vkCreateImageView: Depth-only image formats can have only the VK_IMAGE_ASPECT_DEPTH_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
        } else  if (vk_format_is_stencil_only(imageFormat)) {
            if ((aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != VK_IMAGE_ASPECT_STENCIL_BIT) {
                std::stringstream ss;
                ss << "vkCreateImageView: Stencil-only image formats must have the VK_IMAGE_ASPECT_STENCIL_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
            if ((aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != aspectMask) {
                std::stringstream ss;
                ss << "vkCreateImageView: Stencil-only image formats can have only the VK_IMAGE_ASPECT_STENCIL_BIT set";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)pCreateInfo->image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
        }
    }

    if (skipCall)
        return VK_ERROR_VALIDATION_FAILED;

    VkResult result = get_dispatch_table(image_device_table_map, device)->CreateImageView(device, pCreateInfo, pView);
    return result;
}

VK_LAYER_EXPORT void VKAPI vkCmdClearColorImage(
    VkCmdBuffer                    cmdBuffer,
    VkImage                        image,
    VkImageLayout                  imageLayout,
    const VkClearColorValue       *pColor,
    uint32_t                       rangeCount,
    const VkImageSubresourceRange *pRanges)
{
    VkBool32 skipCall = VK_FALSE;

    // For each range, image aspect must be color only
    for (uint32_t i = 0; i < rangeCount; i++) {
        if (pRanges[i].aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdClearColorImage aspectMasks for all subresource ranges must be set to VK_IMAGE_ASPECT_COLOR_BIT";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdClearColorImage(cmdBuffer, image, imageLayout,
                                                                                  pColor, rangeCount, pRanges);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdClearDepthStencilImage(
    VkCmdBuffer                     cmdBuffer,
    VkImage                         image,
    VkImageLayout                   imageLayout,
    const VkClearDepthStencilValue *pDepthStencil,
    uint32_t                        rangeCount,
    const VkImageSubresourceRange  *pRanges)
{
    VkBool32 skipCall = VK_FALSE;

    // For each range, Image aspect must be depth or stencil or both
    for (uint32_t i = 0; i < rangeCount; i++) {
        if (((pRanges[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)   != VK_IMAGE_ASPECT_DEPTH_BIT) &&
            ((pRanges[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != VK_IMAGE_ASPECT_STENCIL_BIT))
        {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdClearDepthStencilImage aspectMasks for all subresource ranges must be "
                               "set to VK_IMAGE_ASPECT_DEPTH_BIT and/or VK_IMAGE_ASPECT_STENCIL_BIT";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdClearDepthStencilImage(cmdBuffer,
            image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyImage(
    VkCmdBuffer        cmdBuffer,
    VkImage            srcImage,
    VkImageLayout      srcImageLayout,
    VkImage            destImage,
    VkImageLayout      destImageLayout,
    uint32_t           regionCount,
    const VkImageCopy *pRegions)
{
    VkBool32 skipCall = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
    auto srcImageEntry = device_data->imageMap.find(srcImage.handle);
    auto destImageEntry = device_data->imageMap.find(destImage.handle);

    // For each region, src and dst number of layers should not be zero
    // For each region, src and dst number of layers must match
    // For each region, src aspect mask must match dest aspect mask
    // For each region, color aspects cannot be mixed with depth/stencil aspects
    for (uint32_t i = 0; i < regionCount; i++) {
        if(pRegions[i].srcSubresource.numLayers == 0)
        {
            char const str[] = "vkCmdCopyImage: number of layers in source subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        if(pRegions[i].destSubresource.numLayers == 0)
        {
            char const str[] = "vkCmdCopyImage: number of layers in destination subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        if(pRegions[i].srcSubresource.numLayers != pRegions[i].destSubresource.numLayers)
        {
            char const str[] = "vkCmdCopyImage: number of layers in source and destination subresources must match";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        if (pRegions[i].srcSubresource.aspect != pRegions[i].destSubresource.aspect) {
            char const str[] = "vkCmdCopyImage: Src and dest aspectMasks for each region must match";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }
        if ((pRegions[i].srcSubresource.aspect & VK_IMAGE_ASPECT_COLOR_BIT) &&
            (pRegions[i].srcSubresource.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
            char const str[] = "vkCmdCopyImage aspectMask cannot specify both COLOR and DEPTH/STENCIL aspects";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if ((srcImageEntry != device_data->imageMap.end())
    && (destImageEntry != device_data->imageMap.end())) {
        if (srcImageEntry->second.imageType != destImageEntry->second.imageType) {
            char const str[] = "vkCmdCopyImage called with unmatched source and dest image types.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_TYPE, "IMAGE", str);
        }
        // Check that format is same size or exact stencil/depth
        if (is_depth_format(srcImageEntry->second.format)) {
            if (srcImageEntry->second.format != destImageEntry->second.format) {
                char const str[] = "vkCmdCopyImage called with unmatched source and dest image depth/stencil formats.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                    (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_FORMAT, "IMAGE", str);
            }
        } else {
            size_t srcSize = vk_format_get_size(srcImageEntry->second.format);
            size_t destSize = vk_format_get_size(destImageEntry->second.format);
            if (srcSize != destSize) {
                char const str[] = "vkCmdCopyImage called with unmatched source and dest image format sizes.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                    (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_FORMAT, "IMAGE", str);
            }
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdCopyImage(cmdBuffer, srcImage,
            srcImageLayout, destImage, destImageLayout, regionCount, pRegions);
    }
}

void VKAPI vkCmdClearAttachments(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    attachmentCount,
    const VkClearAttachment*                    pAttachments,
    uint32_t                                    rectCount,
    const VkClearRect*                          pRects)
{
    VkBool32 skipCall = VK_FALSE;
    VkImageAspectFlags aspectMask;

    for (uint32_t i = 0; i < attachmentCount; i++) {
        aspectMask = pAttachments[i].aspectMask;
        if (aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            if (aspectMask != VK_IMAGE_ASPECT_COLOR_BIT) {
                // VK_IMAGE_ASPECT_COLOR_BIT is not the only bit set for this attachment
                layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
                char const str[] = "vkCmdClearAttachments aspectMask [%d] must set only VK_IMAGE_ASPECT_COLOR_BIT of a color attachment.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str, i);
            }
        } else {
            // Image aspect must be depth or stencil or both
            if (((aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)   != VK_IMAGE_ASPECT_DEPTH_BIT) &&
                ((aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != VK_IMAGE_ASPECT_STENCIL_BIT))
            {
                layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
                char const str[] = "vkCmdClearAttachments aspectMask [%d] must be set to VK_IMAGE_ASPECT_DEPTH_BIT and/or VK_IMAGE_ASPECT_STENCIL_BIT";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                    (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str, i);
            }
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdClearAttachments(cmdBuffer,
            attachmentCount, pAttachments, rectCount, pRects);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyImageToBuffer(
    VkCmdBuffer              cmdBuffer,
    VkImage                  srcImage,
    VkImageLayout            srcImageLayout,
    VkBuffer                 destBuffer,
    uint32_t                 regionCount,
    const VkBufferImageCopy *pRegions)
{
    VkBool32 skipCall = VK_FALSE;

    // For each region, the number of layers in the image subresource should not be zero
    // Image aspect must be ONE OF color, depth, stencil
    for (uint32_t i = 0; i < regionCount; i++) {
        if(pRegions[i].imageSubresource.numLayers == 0)
        {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdCopyImageToBuffer: number of layers in image subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        VkImageAspectFlags aspect = pRegions[i].imageSubresource.aspect;
        if ((aspect != VK_IMAGE_ASPECT_COLOR_BIT) &&
            (aspect != VK_IMAGE_ASPECT_DEPTH_BIT) &&
            (aspect != VK_IMAGE_ASPECT_STENCIL_BIT)) {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdCopyImageToBuffer: aspectMasks for each region must specify only COLOR or DEPTH or STENCIL";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdCopyImageToBuffer(cmdBuffer,
            srcImage, srcImageLayout, destBuffer, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdCopyBufferToImage(
    VkCmdBuffer              cmdBuffer,
    VkBuffer                 srcBuffer,
    VkImage                  destImage,
    VkImageLayout            destImageLayout,
    uint32_t                 regionCount,
    const VkBufferImageCopy *pRegions)
{
    VkBool32 skipCall = VK_FALSE;

    // For each region, the number of layers in the image subresource should not be zero
    // Image aspect must be ONE OF color, depth, stencil
    for (uint32_t i = 0; i < regionCount; i++) {
        if(pRegions[i].imageSubresource.numLayers == 0)
        {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdCopyBufferToImage: number of layers in image subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        VkImageAspectFlags aspect = pRegions[i].imageSubresource.aspect;
        if ((aspect != VK_IMAGE_ASPECT_COLOR_BIT) &&
            (aspect != VK_IMAGE_ASPECT_DEPTH_BIT) &&
            (aspect != VK_IMAGE_ASPECT_STENCIL_BIT)) {
            layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
            char const str[] = "vkCmdCopyBufferToImage: aspectMasks for each region must specify only COLOR or DEPTH or STENCIL";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdCopyBufferToImage(cmdBuffer,
            srcBuffer, destImage, destImageLayout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkCmdBlitImage(
    VkCmdBuffer        cmdBuffer,
    VkImage            srcImage,
    VkImageLayout      srcImageLayout,
    VkImage            destImage,
    VkImageLayout      destImageLayout,
    uint32_t           regionCount,
    const VkImageBlit *pRegions,
    VkTexFilter        filter)
{
    VkBool32    skipCall    = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);

    auto srcImageEntry  = device_data->imageMap.find(srcImage.handle);
    auto destImageEntry = device_data->imageMap.find(destImage.handle);

    if ((srcImageEntry  != device_data->imageMap.end()) &&
        (destImageEntry != device_data->imageMap.end())) {

        VkFormat srcFormat = srcImageEntry->second.format;
        VkFormat dstFormat = destImageEntry->second.format;

        // Validate consistency for signed and unsigned formats
        if ((vk_format_is_sint(srcFormat) && !vk_format_is_sint(dstFormat)) ||
            (vk_format_is_uint(srcFormat) && !vk_format_is_uint(dstFormat))) {
            std::stringstream ss;
            ss << "vkCmdBlitImage: If one of srcImage and destImage images has signed/unsigned integer format, "
               << "the other one must also have signed/unsigned integer format.  "
               << "Source format is " << string_VkFormat(srcFormat) << " Destinatino format is " << string_VkFormat(dstFormat);
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                               (uint64_t)cmdBuffer, 0, IMAGE_INVALID_FORMAT, "IMAGE", ss.str().c_str());
        }

        // Validate aspect bits and formats for depth/stencil images
        if (vk_format_is_depth_or_stencil(srcFormat) ||
            vk_format_is_depth_or_stencil(dstFormat)) {
            if (srcFormat != dstFormat) {
                std::stringstream ss;
                ss << "vkCmdBlitImage: If one of srcImage and destImage images has a format of depth, stencil or depth "
                   << "stencil, the other one must have exactly the same format.  "
                   << "Source format is " << string_VkFormat(srcFormat) << " Destinatino format is " << string_VkFormat(dstFormat);
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                   (uint64_t)cmdBuffer, 0, IMAGE_INVALID_FORMAT, "IMAGE", ss.str().c_str());
            }

            for (uint32_t i = 0; i < regionCount; i++) {
                if(pRegions[i].srcSubresource.numLayers == 0)
                {
                    char const str[] = "vkCmdBlitImage: number of layers in source subresource is zero";
                    skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                        (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
                }

                if(pRegions[i].destSubresource.numLayers == 0)
                {
                    char const str[] = "vkCmdBlitImage: number of layers in destination subresource is zero";
                    skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                        (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
                }

                if(pRegions[i].srcSubresource.numLayers != pRegions[i].destSubresource.numLayers)
                {
                    char const str[] = "vkCmdBlitImage: number of layers in source and destination subresources must match";
                    skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                        (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
                }

                VkImageAspectFlags srcAspect = pRegions[i].srcSubresource.aspect;
                VkImageAspectFlags dstAspect = pRegions[i].destSubresource.aspect;

                if (srcAspect != dstAspect) {
                    std::stringstream ss;
                    ss << "vkCmdBlitImage: Image aspects of depth/stencil images should match";
                    skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                       (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
                }
                if (vk_format_is_depth_and_stencil(srcFormat)) {
                    if (srcAspect != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
                        std::stringstream ss;
                        ss << "vkCmdBlitImage: Combination depth/stencil image formats must have both VK_IMAGE_ASPECT_DEPTH_BIT "
                           << "and VK_IMAGE_ASPECT_STENCIL_BIT set in both the srcImage and destImage";
                        skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                           (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
                    }
                } else if (vk_format_is_stencil_only(srcFormat)) {
                    if (srcAspect != VK_IMAGE_ASPECT_STENCIL_BIT) {
                        std::stringstream ss;
                        ss << "vkCmdBlitImage: Stencil-only image formats must have only the VK_IMAGE_ASPECT_STENCIL_BIT "
                           << "set in both the srcImage and destImage";
                        skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                           (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
                    }
                } else if (vk_format_is_depth_only(srcFormat)) {
                    if (srcAspect != VK_IMAGE_ASPECT_DEPTH_BIT) {
                        std::stringstream ss;
                        ss << "vkCmdBlitImage: Depth-only image formats must have only the VK_IMAGE_ASPECT_DEPTH "
                           << "set in both the srcImage and destImage";
                        skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                           (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
                    }
                }
            }
        }

        // Validate filter
        if (vk_format_is_depth_or_stencil(srcFormat) ||
            vk_format_is_int(srcFormat)) {
            if (filter != VK_TEX_FILTER_NEAREST) {
                std::stringstream ss;
                ss << "vkCmdBlitImage: If the format of srcImage is a depth, stencil, depth stencil or integer-based format "
                   << "then filter must be VK_TEX_FILTER_NEAREST.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                   (uint64_t)cmdBuffer, 0, IMAGE_INVALID_FILTER, "IMAGE", ss.str().c_str());
            }
        }
    }

    get_dispatch_table(image_device_table_map, cmdBuffer)->CmdBlitImage(cmdBuffer, srcImage,
        srcImageLayout, destImage, destImageLayout, regionCount, pRegions, filter);
}

VK_LAYER_EXPORT void VKAPI vkCmdResolveImage(
    VkCmdBuffer           cmdBuffer,
    VkImage               srcImage,
    VkImageLayout         srcImageLayout,
    VkImage               destImage,
    VkImageLayout         destImageLayout,
    uint32_t              regionCount,
    const VkImageResolve *pRegions)
{
    VkBool32 skipCall = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(cmdBuffer), layer_data_map);
    auto srcImageEntry = device_data->imageMap.find(srcImage.handle);
    auto destImageEntry = device_data->imageMap.find(destImage.handle);

    // For each region, the number of layers in the image subresource should not be zero
    // For each region, src and dest image aspect must be color only
    for (uint32_t i = 0; i < regionCount; i++) {
        if(pRegions[i].srcSubresource.numLayers == 0)
        {
            char const str[] = "vkCmdResolveImage: number of layers in source subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        if(pRegions[i].destSubresource.numLayers == 0)
        {
            char const str[] = "vkCmdResolveImage: number of layers in destination subresource is zero";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_WARN_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_ASPECT, "IMAGE", str);
        }

        if ((pRegions[i].srcSubresource.aspect  != VK_IMAGE_ASPECT_COLOR_BIT) ||
            (pRegions[i].destSubresource.aspect != VK_IMAGE_ASPECT_COLOR_BIT)) {
            char const str[] = "vkCmdResolveImage: src and dest aspectMasks for each region must specify only VK_IMAGE_ASPECT_COLOR_BIT";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", str);
        }
    }

    if ((srcImageEntry  != device_data->imageMap.end()) &&
        (destImageEntry != device_data->imageMap.end())) {
        if (srcImageEntry->second.format != destImageEntry->second.format) {
            char const str[] =  "vkCmdResolveImage called with unmatched source and dest formats.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_FORMAT, "IMAGE", str);
        }
        if (srcImageEntry->second.imageType != destImageEntry->second.imageType) {
            char const str[] =  "vkCmdResolveImage called with unmatched source and dest image types.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_MISMATCHED_IMAGE_TYPE, "IMAGE", str);
        }
        if (srcImageEntry->second.samples <= 1) {
            char const str[] =  "vkCmdResolveImage called with source sample count less than 2.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_RESOLVE_SAMPLES,  "IMAGE", str);
        }
        if (destImageEntry->second.samples > 1) {
            char const str[] =  "vkCmdResolveImage called with dest sample count greater than 1.";
            skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_COMMAND_BUFFER,
                                (uint64_t)cmdBuffer, 0, IMAGE_INVALID_RESOLVE_SAMPLES, "IMAGE", str);
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, cmdBuffer)->CmdResolveImage(cmdBuffer, srcImage,
            srcImageLayout, destImage, destImageLayout, regionCount, pRegions);
    }
}

VK_LAYER_EXPORT void VKAPI vkGetImageSubresourceLayout(
    VkDevice                  device,
    VkImage                   image,
    const VkImageSubresource *pSubresource,
    VkSubresourceLayout      *pLayout)
{
    VkBool32    skipCall    = VK_FALSE;
    layer_data *device_data = get_my_data_ptr(get_dispatch_key(device), layer_data_map);
    VkFormat    format;

    auto imageEntry = device_data->imageMap.find(image.handle);

    // Validate that image aspects match formats
    if (imageEntry != device_data->imageMap.end()) {
        format = imageEntry->second.format;
        if (vk_format_is_color(format)) {
            if (pSubresource->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
                std::stringstream ss;
                ss << "vkGetImageSubresourceLayout: For color formats, the aspect field of VkImageSubresource must be VK_IMAGE_ASPECT_COLOR.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
        } else if (vk_format_is_depth_or_stencil(format)) {
            if ((pSubresource->aspect != VK_IMAGE_ASPECT_DEPTH_BIT) &&
                (pSubresource->aspect != VK_IMAGE_ASPECT_STENCIL_BIT)) {
                std::stringstream ss;
                ss << "vkGetImageSubresourceLayout: For depth/stencil formats, the aspect selects either the depth or stencil image aspect.";
                skipCall |= log_msg(device_data->report_data, VK_DBG_REPORT_ERROR_BIT, VK_OBJECT_TYPE_IMAGE,
                                   (uint64_t)image.handle, 0, IMAGE_INVALID_IMAGE_ASPECT, "IMAGE", ss.str().c_str());
            }
        }
    }

    if (VK_FALSE == skipCall) {
        get_dispatch_table(image_device_table_map, device)->GetImageSubresourceLayout(device,
                           image, pSubresource, pLayout);
    }

}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI vkGetDeviceProcAddr(VkDevice device, const char* funcName)
{
    if (device == NULL) {
        return NULL;
    }

    // loader uses this to force layer initialization; device object is wrapped
    if (!strcmp(funcName, "vkGetDeviceProcAddr")) {
        initDeviceTable(image_device_table_map, (const VkBaseLayerObject *) device);
        return (PFN_vkVoidFunction) vkGetDeviceProcAddr;
    }

    if (!strcmp(funcName, "vkCreateDevice"))
        return (PFN_vkVoidFunction) vkCreateDevice;
    if (!strcmp(funcName, "vkDestroyDevice"))
        return (PFN_vkVoidFunction) vkDestroyDevice;
    if (!strcmp(funcName, "vkCreateImage"))
        return (PFN_vkVoidFunction) vkCreateImage;
    if (!strcmp(funcName, "vkDestroyImage"))
        return (PFN_vkVoidFunction) vkDestroyImage;
    if (!strcmp(funcName, "vkCreateImageView"))
        return (PFN_vkVoidFunction) vkCreateImageView;
    if (!strcmp(funcName, "vkCreateRenderPass"))
        return (PFN_vkVoidFunction) vkCreateRenderPass;
    if (!strcmp(funcName, "vkCmdClearColorImage"))
        return (PFN_vkVoidFunction) vkCmdClearColorImage;
    if (!strcmp(funcName, "vkCmdClearDepthStencilImage"))
        return (PFN_vkVoidFunction) vkCmdClearDepthStencilImage;
    if (!strcmp(funcName, "vkCmdClearAttachments"))
        return (PFN_vkVoidFunction) vkCmdClearAttachments;
    if (!strcmp(funcName, "vkCmdCopyImage"))
        return (PFN_vkVoidFunction) vkCmdCopyImage;
    if (!strcmp(funcName, "vkCmdCopyImageToBuffer"))
        return (PFN_vkVoidFunction) vkCmdCopyImageToBuffer;
    if (!strcmp(funcName, "vkCmdCopyBufferToImage"))
        return (PFN_vkVoidFunction) vkCmdCopyBufferToImage;
    if (!strcmp(funcName, "vkCmdBlitImage"))
        return (PFN_vkVoidFunction) vkCmdBlitImage;
    if (!strcmp(funcName, "vkCmdResolveImage"))
        return (PFN_vkVoidFunction) vkCmdResolveImage;
    if (!strcmp(funcName, "vkGetImageSubresourceLayout"))
        return (PFN_vkVoidFunction) vkGetImageSubresourceLayout;
    {
        if (get_dispatch_table(image_device_table_map, device)->GetDeviceProcAddr == NULL)
            return NULL;
        return get_dispatch_table(image_device_table_map, device)->GetDeviceProcAddr(device, funcName);
    }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI vkGetInstanceProcAddr(VkInstance instance, const char* funcName)
{
    if (instance == NULL) {
        return NULL;
    }

    // loader uses this to force layer initialization; instance object is wrapped
    if (!strcmp(funcName, "vkGetInstanceProcAddr")) {
        initInstanceTable(image_instance_table_map, (const VkBaseLayerObject *) instance);
        return (PFN_vkVoidFunction) vkGetInstanceProcAddr;
    }

    if (!strcmp(funcName, "vkCreateInstance"))
        return (PFN_vkVoidFunction) vkCreateInstance;
    if (!strcmp(funcName, "vkDestroyInstance"))
        return (PFN_vkVoidFunction) vkDestroyInstance;
    if (!strcmp(funcName, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceLayerProperties;
    if (!strcmp(funcName, "vkEnumerateInstanceExtensionProperties"))
        return (PFN_vkVoidFunction) vkEnumerateInstanceExtensionProperties;
    if (!strcmp(funcName, "vkEnumerateDeviceLayerProperties"))
        return (PFN_vkVoidFunction) vkEnumerateDeviceLayerProperties;
    if (!strcmp(funcName, "vkEnumerateDeviceExtensionProperties"))
        return (PFN_vkVoidFunction) vkEnumerateDeviceExtensionProperties;

    layer_data *data = get_my_data_ptr(get_dispatch_key(instance), layer_data_map);
    PFN_vkVoidFunction fptr = debug_report_get_instance_proc_addr(data->report_data, funcName);
    if(fptr)
        return fptr;

    {
        if (get_dispatch_table(image_instance_table_map, instance)->GetInstanceProcAddr == NULL)
            return NULL;
        return get_dispatch_table(image_instance_table_map, instance)->GetInstanceProcAddr(instance, funcName);
    }
}
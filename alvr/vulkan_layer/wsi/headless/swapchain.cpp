/*
 * Copyright (c) 2017-2020 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file swapchain.cpp
 *
 * @brief Contains the implementation for a headless swapchain.
 */

#include <cassert>
#include <cstdlib>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <sys/mman.h>

#include <util/timed_semaphore.hpp>

#include "util/logger.h"
#include "platform/linux/protocol.h"
#include "swapchain.hpp"
#include "wsi/display.hpp"

namespace wsi {
namespace headless {

static bool supported_depth_format(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return true;
    default:
        return false;
    }
}

struct image_data {
    /* Device memory backing the image. */
    VkDeviceMemory memory;
};

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator)
    : wsi::swapchain_base(dev_data, pAllocator), m_display(*dev_data.display) {}

swapchain::~swapchain() {
    /* Call the base's teardown */
    close(m_socket);
    teardown();
    if (m_depth_command_pool != VK_NULL_HANDLE) {
        m_device_data.disp.DestroyCommandPool(m_device, m_depth_command_pool, nullptr);
        m_depth_command_pool = VK_NULL_HANDLE;
    }
}

swapchain::depth_proxy_image *swapchain::find_depth_proxy(VkImage color_image) {
    for (auto &depth : m_depth_images) {
        if (depth.color_image == color_image) {
            return &depth;
        }
    }
    return nullptr;
}

bool swapchain::create_depth_proxy(
    const VkImageCreateInfo &image_create_info,
    VkImage color_image,
    VkFormat depth_format
) {
    if (m_depth_command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = 0;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (m_device_data.disp.CreateCommandPool(m_device, &pool_info, nullptr, &m_depth_command_pool)
            != VK_SUCCESS) {
            return false;
        }
    }

    depth_proxy_image proxy = {};
    proxy.color_image = color_image;

    m_depth_create_info = image_create_info;
    m_depth_create_info.format = depth_format;
    m_depth_create_info.usage
        = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    m_depth_create_info.pNext = nullptr;
    m_depth_create_info.pQueueFamilyIndices = nullptr;
    m_depth_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkExternalMemoryImageCreateInfo ext_image_info = {};
    ext_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    m_depth_create_info.pNext = &ext_image_info;

    if (m_device_data.disp.CreateImage(m_device, &m_depth_create_info, nullptr, &proxy.image)
        != VK_SUCCESS) {
        return false;
    }
    m_depth_create_info.pNext = nullptr;

    VkMemoryRequirements memory_requirements;
    m_device_data.disp.GetImageMemoryRequirements(m_device, proxy.image, &memory_requirements);

    size_t mem_type_idx = 0;
    VkMemoryPropertyFlags memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkPhysicalDeviceMemoryProperties prop;
    m_device_data.instance_data.disp.GetPhysicalDeviceMemoryProperties(
        m_device_data.physical_device, &prop
    );
    for (; mem_type_idx < prop.memoryTypeCount; ++mem_type_idx) {
        if ((prop.memoryTypes[mem_type_idx].propertyFlags & memFlags) == memFlags
            && (memory_requirements.memoryTypeBits & (1 << mem_type_idx))) {
            break;
        }
    }
    if (mem_type_idx >= prop.memoryTypeCount) {
        return false;
    }

    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryDedicatedAllocateInfo ded_info = {};
    ded_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded_info.image = proxy.image;
    ded_info.pNext = &export_info;

    VkMemoryAllocateInfo mem_info = {};
    mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_info.allocationSize = memory_requirements.size;
    mem_info.memoryTypeIndex = mem_type_idx;
    mem_info.pNext = &ded_info;
    m_depth_mem_index = mem_type_idx;

    if (m_device_data.disp.AllocateMemory(m_device, &mem_info, nullptr, &proxy.memory) != VK_SUCCESS) {
        return false;
    }
    if (m_device_data.disp.BindImageMemory(m_device, proxy.image, proxy.memory, 0) != VK_SUCCESS) {
        return false;
    }

    VkMemoryGetFdInfoKHR memory_fd_info = {};
    memory_fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    memory_fd_info.memory = proxy.memory;
    memory_fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (m_device_data.disp.GetMemoryFdKHR(m_device, &memory_fd_info, &fd) != VK_SUCCESS) {
        return false;
    }
    m_depth_fds.push_back(fd);

    VkExportSemaphoreCreateInfo exp_sem_info = {};
    exp_sem_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_sem_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreTypeCreateInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.pNext = &exp_sem_info;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo sem_info = {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &timeline_info;

    if (m_device_data.disp.CreateSemaphore(m_device, &sem_info, nullptr, &proxy.semaphore)
        != VK_SUCCESS) {
        return false;
    }

    VkSemaphoreGetFdInfoKHR sem_fd_info = {};
    sem_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sem_fd_info.semaphore = proxy.semaphore;
    sem_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    if (m_device_data.disp.GetSemaphoreFdKHR(m_device, &sem_fd_info, &fd) != VK_SUCCESS) {
        return false;
    }
    m_depth_fds.push_back(fd);

    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = m_depth_command_pool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    if (m_device_data.disp.AllocateCommandBuffers(m_device, &cb_info, &proxy.command_buffer)
        != VK_SUCCESS) {
        return false;
    }

    m_depth_images.push_back(proxy);
    return true;
}

bool swapchain::copy_native_depth(uint32_t pending_index, uint64_t color_semaphore_value) {
    if (pending_index >= m_swapchain_images.size()) {
        return false;
    }

    const VkImage color_image = m_swapchain_images[pending_index].image;
    auto *proxy = find_depth_proxy(color_image);
    if (proxy == nullptr || proxy->command_buffer == VK_NULL_HANDLE) {
        return false;
    }

    layer::device_private_data::depth_source_info source_info;
    if (!m_device_data.get_depth_source_for_color_image(color_image, source_info) || !source_info.valid) {
        return false;
    }
    if (source_info.layout == VK_IMAGE_LAYOUT_UNDEFINED
        || !supported_depth_format(source_info.format)
        || source_info.format != m_depth_create_info.format) {
        return false;
    }

    if (m_device_data.disp.ResetCommandBuffer(proxy->command_buffer, 0) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (m_device_data.disp.BeginCommandBuffer(proxy->command_buffer, &begin_info) != VK_SUCCESS) {
        return false;
    }

    VkImageMemoryBarrier barriers[3] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = source_info.layout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].image = source_info.image;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barriers[0].subresourceRange.levelCount = 1;
    barriers[0].subresourceRange.layerCount = 1;

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].oldLayout = proxy->layout;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].image = proxy->image;
    barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barriers[1].subresourceRange.levelCount = 1;
    barriers[1].subresourceRange.layerCount = 1;

    m_device_data.disp.CmdPipelineBarrier(
        proxy->command_buffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        2,
        barriers
    );

    VkImageCopy copy_region = {};
    copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy_region.srcSubresource.layerCount = 1;
    copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy_region.dstSubresource.layerCount = 1;
    copy_region.extent = m_depth_create_info.extent;
    m_device_data.disp.CmdCopyImage(
        proxy->command_buffer,
        source_info.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        proxy->image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region
    );

    barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[2].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[2].newLayout = source_info.layout;
    barriers[2].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[2].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    barriers[2].image = source_info.image;
    barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barriers[2].subresourceRange.levelCount = 1;
    barriers[2].subresourceRange.layerCount = 1;
    m_device_data.disp.CmdPipelineBarrier(
        proxy->command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barriers[2]
    );

    if (m_device_data.disp.EndCommandBuffer(proxy->command_buffer) != VK_SUCCESS) {
        return false;
    }

    const uint64_t depth_semaphore_value = ++proxy->semaphore_value;
    VkTimelineSemaphoreSubmitInfo timeline_info = {};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = 1;
    timeline_info.pWaitSemaphoreValues = &color_semaphore_value;
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = &depth_semaphore_value;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &m_swapchain_images[pending_index].semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &proxy->command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &proxy->semaphore;
    if (m_device_data.disp.QueueSubmit(m_queue, 1, &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
        return false;
    }

    proxy->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return true;
}

VkResult swapchain::create_image(const VkImageCreateInfo &image_create,
                                 wsi::swapchain_image &image) {
    VkResult res = VK_SUCCESS;
    m_create_info = image_create;
    m_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT
      | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_SAMPLED_BIT
      | VK_IMAGE_USAGE_STORAGE_BIT;
    res = m_device_data.disp.CreateImage(m_device, &m_create_info, nullptr, &image.image);
    if (res != VK_SUCCESS) {
        return res;
    }
    m_create_info.pNext = nullptr;
    m_create_info.pQueueFamilyIndices = nullptr;

    VkMemoryRequirements memory_requirements;
    m_device_data.disp.GetImageMemoryRequirements(m_device, image.image, &memory_requirements);

    /* Find a memory type */
    size_t mem_type_idx = 0;
    VkMemoryPropertyFlags memFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkPhysicalDeviceMemoryProperties prop;
    m_device_data.instance_data.disp.GetPhysicalDeviceMemoryProperties(m_device_data.physical_device, &prop);
    for (; mem_type_idx < prop.memoryTypeCount; ++mem_type_idx) {
        if ((prop.memoryTypes[mem_type_idx].propertyFlags & memFlags) == memFlags && memory_requirements.memoryTypeBits & (1 << mem_type_idx)) {
            break;
        }
    }
    assert(mem_type_idx < prop.memoryTypeCount);

    VkExportMemoryAllocateInfo export_info = {};
    export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryDedicatedAllocateInfo ded_info = {};
    ded_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded_info.image = image.image;
    ded_info.pNext = &export_info;

    VkMemoryAllocateInfo mem_info = {};
    mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_info.allocationSize = memory_requirements.size;
    mem_info.memoryTypeIndex = mem_type_idx;
    mem_info.pNext = &ded_info;
    m_mem_index = mem_type_idx;
    image_data *data = nullptr;

    /* Create image_data */
    data = m_allocator.create<image_data>(1);
    if (data == nullptr) {
        m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    image.data = reinterpret_cast<void *>(data);
    image.status = wsi::swapchain_image::FREE;

    res = m_device_data.disp.AllocateMemory(m_device, &mem_info, nullptr, &data->memory);
    assert(VK_SUCCESS == res);
    if (res != VK_SUCCESS) {
        destroy_image(image);
        return res;
    }

    res = m_device_data.disp.BindImageMemory(m_device, image.image, data->memory, 0);
    assert(VK_SUCCESS == res);
    if (res != VK_SUCCESS) {
        destroy_image(image);
        return res;
    }
    m_device_data.register_layer_swapchain_image(image.image);

    /* Initialize presentation fence. */
    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
    res = m_device_data.disp.CreateFence(m_device, &fence_info, nullptr, &image.present_fence);
    if (res != VK_SUCCESS) {
        destroy_image(image);
        return res;
    }

    // Export into a FD to send later
    VkMemoryGetFdInfoKHR fd_info = {};
    fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fd_info.pNext = NULL;
    fd_info.memory = data->memory;
    fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd;
    res = m_device_data.disp.GetMemoryFdKHR(m_device, &fd_info, &fd);
    if (res != VK_SUCCESS) {
        Error("GetMemoryFdKHR failed\n");
        destroy_image(image);
        return res;
    }
    m_fds.push_back(fd);
    Debug("GetMemoryFdKHR returned fd=%d\n", fd);

    VkExportSemaphoreCreateInfo exp_info = {};
    exp_info.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    exp_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkSemaphoreTypeCreateInfo tim_info = {};
    tim_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    tim_info.pNext = &exp_info;
    tim_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo sem_info = {};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sem_info.pNext = &tim_info;

    res = m_device_data.disp.CreateSemaphore(m_device, &sem_info, nullptr, &image.semaphore);
    if (res != VK_SUCCESS) {
        Error("CreateSemaphore failed\n");
        destroy_image(image);
        return res;
    }

    VkSemaphoreGetFdInfoKHR sem_fd_info = {};
    sem_fd_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    sem_fd_info.semaphore = image.semaphore;
    sem_fd_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    res = m_device_data.disp.GetSemaphoreFdKHR(m_device, &sem_fd_info, &fd);
    if (res != VK_SUCCESS) {
        Error("GetSemaphoreFdKHR failed\n");
        destroy_image(image);
        return res;
    }
    m_fds.push_back(fd);
    Debug("GetSemaphoreFdKHR returned fd=%d\n", fd);

    if (!create_depth_proxy(image_create, image.image, VK_FORMAT_D32_SFLOAT)) {
        Error("Failed to create depth proxy image\n");
        destroy_image(image);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    return res;
}

int swapchain::send_fds() {
    // This function does the arcane magic for sending
    // file descriptors over unix domain sockets
    // Stolen from https://gist.github.com/kokjo/75cec0f466fc34fa2922
    //
    struct msghdr msg;
    struct iovec iov[1];
    struct cmsghdr *cmsg = NULL;
    std::vector<int> fds(m_fds.begin(), m_fds.end());
    fds.insert(fds.end(), m_depth_fds.begin(), m_depth_fds.end());
    std::vector<char> ctrl_buf(CMSG_SPACE(sizeof(int) * fds.size()));
    char data[1];

    memset(&msg, 0, sizeof(struct msghdr));
    memset(ctrl_buf.data(), 0, ctrl_buf.size());

    iov[0].iov_base = data;
    iov[0].iov_len = sizeof(data);

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_controllen = ctrl_buf.size();
    msg.msg_control = ctrl_buf.data();

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());

    memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());

    int ret = sendmsg(m_socket, &msg, 0);

    for (auto fd: fds)
        close(fd);

    return ret;
}

bool swapchain::try_connect() {
    Debug("swapchain::try_connect\n");
    m_socketPath = getenv("XDG_RUNTIME_DIR");
    m_socketPath += "/alvr-ipc";

    int ret;
    if (m_socket == -1) {
        m_socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (m_socket == -1) {
            perror("socket");
            exit(1);
        }
    }

    struct sockaddr_un name;
    memset(&name, 0, sizeof(name));
    name.sun_family = AF_UNIX;
    strncpy(name.sun_path, m_socketPath.c_str(), sizeof(name.sun_path) - 1);

    VkFormat selected_depth_format = supported_depth_format(m_depth_create_info.format)
        ? m_depth_create_info.format
        : VK_FORMAT_D32_SFLOAT;
    for (const auto &image : m_swapchain_images) {
        layer::device_private_data::depth_source_info source_info;
        if (m_device_data.get_depth_source_for_color_image(image.image, source_info)
            && source_info.valid && source_info.layout != VK_IMAGE_LAYOUT_UNDEFINED
            && supported_depth_format(source_info.format)) {
            selected_depth_format = source_info.format;
            break;
        }
    }

    if (m_depth_images.size() != m_swapchain_images.size()
        || m_depth_create_info.format != selected_depth_format) {
        for (auto &proxy : m_depth_images) {
            if (proxy.command_buffer != VK_NULL_HANDLE) {
                m_device_data.disp.FreeCommandBuffers(
                    m_device, m_depth_command_pool, 1, &proxy.command_buffer
                );
            }
            if (proxy.semaphore != VK_NULL_HANDLE) {
                m_device_data.disp.DestroySemaphore(m_device, proxy.semaphore, nullptr);
            }
            if (proxy.image != VK_NULL_HANDLE) {
                m_device_data.disp.DestroyImage(m_device, proxy.image, get_allocation_callbacks());
            }
            if (proxy.memory != VK_NULL_HANDLE) {
                m_device_data.disp.FreeMemory(m_device, proxy.memory, nullptr);
            }
        }
        m_depth_images.clear();

        for (int fd : m_depth_fds) {
            close(fd);
        }
        m_depth_fds.clear();

        for (const auto &image : m_swapchain_images) {
            if (!create_depth_proxy(m_create_info, image.image, selected_depth_format)) {
                Error(
                    "Failed to create depth proxy image with format %d\n", int(selected_depth_format)
                );
                return false;
            }
        }
    }

    ret = connect(m_socket, (const struct sockaddr *)&name, sizeof(name));
    if (ret == -1) {
        return false; // we will try again next frame
    }

    VkPhysicalDeviceVulkan11Properties props11 = {};
    props11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;

    VkPhysicalDeviceProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &props11;
    m_device_data.instance_data.disp.GetPhysicalDeviceProperties2(m_device_data.physical_device,
                                                                  &props);

    init_packet init{.num_images = uint32_t(m_swapchain_images.size()),
      .has_depth = 1,
      .depth_is_color = 0,
      ._padding = {},
      .device_uuid = {},
      .image_create_info = m_create_info,
      .mem_index = m_mem_index,
      .depth_image_create_info = m_depth_create_info,
      .depth_mem_index = m_depth_mem_index,
      .source_pid = getpid()};
    memcpy(init.device_uuid.data(), props11.deviceUUID, VK_UUID_SIZE);
    ret = write(m_socket, &init, sizeof(init));
    if (ret == -1) {
        perror("write");
        exit(1);
    }

    ret = send_fds();
    if (ret == -1) {
        perror("sendmsg");
        exit(1);
    }
    Debug("swapchain sent fds\n");

    return true;
}

void swapchain::submit_image(uint32_t pending_index) {
    const auto & pose = m_swapchain_images[pending_index].pose.mDeviceToAbsoluteTracking.m;
    if (!m_connected) {
        m_connected = try_connect();
    }
    if (m_connected) {
        int ret;
        present_packet packet;
        packet.image = pending_index;
        packet.frame = m_display.m_vsync_count;
        packet.semaphore_value = m_swapchain_images[pending_index].semaphore_value;
        packet.depth_image = pending_index;
        packet.depth_semaphore_value = 0;
        packet.has_depth = 0;
        packet.depth_is_color = 0;
        if (copy_native_depth(pending_index, packet.semaphore_value)) {
            if (auto *proxy = find_depth_proxy(m_swapchain_images[pending_index].image)) {
                packet.depth_semaphore_value = proxy->semaphore_value;
                packet.has_depth = 1;
            }
        }
        memset(packet._padding, 0, sizeof(packet._padding));
        memcpy(&packet.pose, pose, sizeof(packet.pose));
        ret = write(m_socket, &packet, sizeof(packet));
        if (ret == -1) {
            //FIXME: try to reconnect?
        }
    }
}

void swapchain::present_image(uint32_t pending_index) {
    if (in_flight_index != UINT32_MAX)
        unpresent_image(in_flight_index);
    in_flight_index = pending_index;
}

void swapchain::destroy_image(wsi::swapchain_image &image) {
    const VkImage color_image = image.image;
    if (image.status != wsi::swapchain_image::INVALID) {
        if (image.present_fence != VK_NULL_HANDLE) {
            m_device_data.disp.DestroyFence(m_device, image.present_fence, nullptr);
            image.present_fence = VK_NULL_HANDLE;
        }

        if (image.image != VK_NULL_HANDLE) {
            m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
            image.image = VK_NULL_HANDLE;
        }
    }

    if (image.data != nullptr) {
        auto *data = reinterpret_cast<image_data *>(image.data);
        if (data->memory != VK_NULL_HANDLE) {
            m_device_data.disp.FreeMemory(m_device, data->memory, nullptr);
            data->memory = VK_NULL_HANDLE;
        }
        m_allocator.destroy(1, data);
        image.data = nullptr;
    }

    for (auto &proxy : m_depth_images) {
        if (proxy.color_image != color_image) {
            continue;
        }
        if (proxy.semaphore != VK_NULL_HANDLE) {
            m_device_data.disp.DestroySemaphore(m_device, proxy.semaphore, nullptr);
            proxy.semaphore = VK_NULL_HANDLE;
        }
        if (proxy.image != VK_NULL_HANDLE) {
            m_device_data.disp.DestroyImage(m_device, proxy.image, get_allocation_callbacks());
            proxy.image = VK_NULL_HANDLE;
        }
        if (proxy.memory != VK_NULL_HANDLE) {
            m_device_data.disp.FreeMemory(m_device, proxy.memory, nullptr);
            proxy.memory = VK_NULL_HANDLE;
        }
        break;
    }

    image.status = wsi::swapchain_image::INVALID;
}

} /* namespace headless */
} /* namespace wsi */

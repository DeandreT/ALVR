#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vulkan/vulkan.h>

struct present_packet {
    uint32_t image;
    uint32_t frame;
    uint64_t semaphore_value;
    uint32_t depth_image;
    uint64_t depth_semaphore_value;
    uint8_t has_depth;
    uint8_t depth_is_color;
    uint8_t _padding[6];
    float pose[3][4];
};

struct init_packet {
    uint32_t num_images;
    uint8_t has_depth;
    uint8_t depth_is_color;
    uint8_t _padding[2];
    std::array<uint8_t, VK_UUID_SIZE> device_uuid;
    VkImageCreateInfo image_create_info;
    size_t mem_index;
    VkImageCreateInfo depth_image_create_info;
    size_t depth_mem_index;
    pid_t source_pid;
};

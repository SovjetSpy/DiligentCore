/*     Copyright 2015-2018 Egor Yusov
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
*
*  In no event and under no legal theory, whether in tort (including negligence),
*  contract, or otherwise, unless required by applicable law (such as deliberate
*  and grossly negligent acts) or agreed to in writing, shall any Contributor be
*  liable for any damages, including any direct, indirect, special, incidental,
*  or consequential damages of any character arising as a result of this License or
*  out of the use or inability to use the software (including but not limited to damages
*  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
*  all other commercial damages or losses), even if such Contributor has been advised
*  of the possibility of such damages.
*/
#include <sstream>

#include "VulkanUtilities/VulkanCommandBufferPool.h"
#include "VulkanUtilities/VulkanDebug.h"
#include "Errors.h"
#include "DebugUtilities.h"
#include "VulkanErrors.h"

namespace VulkanUtilities
{
    VulkanCommandBufferPool::VulkanCommandBufferPool(std::shared_ptr<const VulkanUtilities::VulkanLogicalDevice> LogicalDevice,
                                                     uint32_t                                                    queueFamilyIndex, 
                                                     VkCommandPoolCreateFlags                                    flags) :
        m_LogicalDevice(LogicalDevice)
    {
        VkCommandPoolCreateInfo CmdPoolCI = {};
        CmdPoolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        CmdPoolCI.pNext = nullptr;
        CmdPoolCI.queueFamilyIndex = queueFamilyIndex;
        CmdPoolCI.flags = flags;
        m_CmdPool = m_LogicalDevice->CreateCommandPool(CmdPoolCI);
        VERIFY_EXPR(m_CmdPool != VK_NULL_HANDLE);
    }

    VulkanCommandBufferPool::~VulkanCommandBufferPool()
    {
        m_CmdPool.Release();
    }

    VkCommandBuffer VulkanCommandBufferPool::GetCommandBuffer(const char* DebugName)
    {
        VkCommandBuffer CmdBuffer = VK_NULL_HANDLE;

        {
            std::lock_guard<std::mutex> Lock(m_Mutex);

            if (!m_CmdBuffers.empty())
            {
                CmdBuffer = m_CmdBuffers.front();
                auto err = vkResetCommandBuffer(CmdBuffer, 
                    0 // VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT -  specifies that most or all memory resources currently 
                        // owned by the command buffer should be returned to the parent command pool.
                );
                DEV_CHECK_ERR(err == VK_SUCCESS, "Failed to reset command buffer");
                m_CmdBuffers.pop_front();
            }
        }

        // If no cmd buffers were ready to be reused, create a new one
        if (CmdBuffer == VK_NULL_HANDLE)
        {
            VkCommandBufferAllocateInfo BuffAllocInfo = {};
            BuffAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            BuffAllocInfo.pNext = nullptr;
            BuffAllocInfo.commandPool = m_CmdPool;
            BuffAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            BuffAllocInfo.commandBufferCount = 1;

            CmdBuffer = m_LogicalDevice->AllocateVkCommandBuffer(BuffAllocInfo);
        }

        VkCommandBufferBeginInfo CmdBuffBeginInfo = {};
        CmdBuffBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        CmdBuffBeginInfo.pNext = nullptr;
        CmdBuffBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; // Each recording of the command buffer will only be 
                                                                              // submitted once, and the command buffer will be reset 
                                                                              // and recorded again between each submission.
        CmdBuffBeginInfo.pInheritanceInfo = nullptr; // Ignored for a primary command buffer
        auto err = vkBeginCommandBuffer(CmdBuffer, &CmdBuffBeginInfo);
        VERIFY(err == VK_SUCCESS, "Failed to begin command buffer");

        return CmdBuffer;
    }

    void VulkanCommandBufferPool::FreeCommandBuffer(VkCommandBuffer&& CmdBuffer)
    {
        std::lock_guard<std::mutex> Lock(m_Mutex);
        // FenceValue is the value that was signaled by the command queue after it 
        // executed the command buffer
        m_CmdBuffers.emplace_back(CmdBuffer);
        CmdBuffer = VK_NULL_HANDLE;
    }

    CommandPoolWrapper&& VulkanCommandBufferPool::Release()
    {
        m_LogicalDevice.reset();
        m_CmdBuffers.clear();
        return std::move(m_CmdPool);
    }
}

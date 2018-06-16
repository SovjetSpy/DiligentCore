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

#include "pch.h"
#include "VulkanDynamicHeap.h"
#include "RenderDeviceVkImpl.h"

namespace Diligent
{

static VkDeviceSize GetDefaultAlignment(const VulkanUtilities::VulkanPhysicalDevice& PhysicalDevice)
{
    const auto& Props = PhysicalDevice.GetProperties();
    const auto& Limits = Props.limits;
    return std::max(std::max(Limits.minUniformBufferOffsetAlignment, Limits.minTexelBufferOffsetAlignment), Limits.minStorageBufferOffsetAlignment);
}

VulkanRingBuffer::VulkanRingBuffer(IMemoryAllocator&      Allocator,
                                     RenderDeviceVkImpl*    pDeviceVk,
                                     Uint32                 Size) :
    m_RingBuffer(Size, Allocator),
    m_pDeviceVk(pDeviceVk),
    m_DefaultAlignment(GetDefaultAlignment(pDeviceVk->GetPhysicalDevice()))
{
    VERIFY( (Size & (MinAlignment-1)) == 0, "Heap size is not min aligned");
    VkBufferCreateInfo VkBuffCI = {};
    VkBuffCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    VkBuffCI.pNext = nullptr;
    VkBuffCI.flags = 0; // VK_BUFFER_CREATE_SPARSE_BINDING_BIT, VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, VK_BUFFER_CREATE_SPARSE_ALIASED_BIT
    VkBuffCI.size = Size;
    VkBuffCI.usage = 
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT    | 
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT  | 
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT  | 
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT    | 
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT   | 
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffCI.queueFamilyIndexCount = 0;
    VkBuffCI.pQueueFamilyIndices = nullptr;

    const auto& LogicalDevice = pDeviceVk->GetLogicalDevice();
    m_VkBuffer = LogicalDevice.CreateBuffer(VkBuffCI, "Dynamic heap buffer");
    VkMemoryRequirements MemReqs = LogicalDevice.GetBufferMemoryRequirements(m_VkBuffer);

    const auto& PhysicalDevice = pDeviceVk->GetPhysicalDevice();

    VkMemoryAllocateInfo MemAlloc = {};
    MemAlloc.pNext = nullptr;
    MemAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    MemAlloc.allocationSize = MemReqs.size;

    // VK_MEMORY_PROPERTY_HOST_COHERENT_BIT bit specifies that the host cache management commands vkFlushMappedMemoryRanges 
    // and vkInvalidateMappedMemoryRanges are NOT needed to flush host writes to the device or make device writes visible
    // to the host (10.2)
    MemAlloc.memoryTypeIndex = PhysicalDevice.GetMemoryTypeIndex(MemReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VERIFY(MemAlloc.memoryTypeIndex != VulkanUtilities::VulkanPhysicalDevice::InvalidMemoryTypeIndex,
           "Vulkan spec requires that for a VkBuffer not created with the "
           "VK_BUFFER_CREATE_SPARSE_BINDING_BIT bit set, the memoryTypeBits member always contains at least one bit set "
           "corresponding to a VkMemoryType with a propertyFlags that has both the VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT bit "
           "and the VK_MEMORY_PROPERTY_HOST_COHERENT_BIT bit set(11.6)");

    m_BufferMemory = LogicalDevice.AllocateDeviceMemory(MemAlloc, "Host-visible memory for upload buffer");

    void *Data = nullptr;
    auto err = LogicalDevice.MapMemory(m_BufferMemory,
        0, // offset
        MemAlloc.allocationSize,
        0, // flags, reserved for future use
        &Data);
    m_CPUAddress = reinterpret_cast<Uint8*>(Data);
    CHECK_VK_ERROR_AND_THROW(err, "Failed to map  memory");

    err = LogicalDevice.BindBufferMemory(m_VkBuffer, m_BufferMemory, 0 /*offset*/);
    CHECK_VK_ERROR_AND_THROW(err, "Failed to bind  bufer memory");

    LOG_INFO_MESSAGE("GPU dynamic heap created. Total buffer size: ", HeapSize);
}

void VulkanRingBuffer::Destroy()
{
    if (m_VkBuffer)
    {
        m_pDeviceVk->GetLogicalDevice().UnmapMemory(m_BufferMemory);
        m_pDeviceVk->SafeReleaseVkObject(std::move(m_VkBuffer));
        m_pDeviceVk->SafeReleaseVkObject(std::move(m_BufferMemory));
    }
    m_CPUAddress = nullptr;
}

VulkanRingBuffer::~VulkanRingBuffer()
{
    VERIFY(m_BufferMemory == VK_NULL_HANDLE && m_VkBuffer == VK_NULL_HANDLE, "Vulkan resources must be explcitly released with Destroy()");
    LOG_INFO_MESSAGE("Dynamic heap ring buffer usage stats:\n"
                     "    Total size: ", SizeFormatter{ m_RingBuffer.GetMaxSize(), 2 },
                     ". Peak allocated size: ", SizeFormatter{ m_TotalPeakSize, 2, m_RingBuffer.GetMaxSize() },
                     ". Peak frame size: ", SizeFormatter{ m_FramePeakSize, 2, m_RingBuffer.GetMaxSize() },
                     ". Peak utilization: ", std::fixed, std::setprecision(1), static_cast<double>(m_TotalPeakSize) / static_cast<double>(std::max(m_RingBuffer.GetMaxSize(), size_t{1})) * 100.0, '%' );
}

RingBuffer::OffsetType VulkanRingBuffer::Allocate(size_t SizeInBytes)
{
    VERIFY( (SizeInBytes & (MinAlignment-1)) == 0, "Allocation size is not minimally aligned" );
    
    if (SizeInBytes > m_RingBuffer.GetMaxSize())
    {
        LOG_ERROR("Requested dynamic allocation size ", SizeInBytes, " exceeds maximum ring buffer size ", m_RingBuffer.GetMaxSize(), ". The app should increase dynamic heap size.");
        return RingBuffer::InvalidOffset;
    }
    
    std::lock_guard<std::mutex> Lock(m_RingBuffMtx);
    auto Offset = m_RingBuffer.Allocate(SizeInBytes);
    if(Offset == RingBuffer::InvalidOffset)
    {
        UNEXPECTED("Allocation failed");
    }
    m_CurrentFrameSize += SizeInBytes;
    m_FramePeakSize = std::max(m_FramePeakSize, m_CurrentFrameSize);
    m_TotalPeakSize = std::max(m_TotalPeakSize, m_RingBuffer.GetUsedSize());
    return Offset;
}

void VulkanRingBuffer::FinishFrame(Uint64 FenceValue, Uint64 LastCompletedFenceValue)
{
    //
    //      Deferred contexts must not map dynamic buffers across several frames!
    //

    std::lock_guard<std::mutex> Lock(m_RingBuffMtx);
    m_RingBuffer.FinishCurrentFrame(FenceValue);
    m_RingBuffer.ReleaseCompletedFrames(LastCompletedFenceValue);
    m_CurrentFrameSize = 0;
}

VulkanDynamicAllocation VulkanDynamicHeap::Allocate(Uint32 SizeInBytes, Uint32 Alignment)
{
    if (Alignment == 0)
        Alignment = static_cast<Uint32>(m_ParentRingBuffer.m_DefaultAlignment);

    const Uint32 AlignmentMask = Alignment - 1;
    // Assert that it's a power of two.
    VERIFY_EXPR((AlignmentMask & Alignment) == 0);

    // Align the allocation
    Uint32 AlignedSize = (SizeInBytes + AlignmentMask) & ~AlignmentMask;

    //
    //      Deferred contexts must not map dynamic buffers across several frames!
    //
    auto Offset = RingBuffer::InvalidOffset;
    if(AlignedSize > m_PagSize)
    {
        // Allocate directly from the ring buffer
        Offset = m_ParentRingBuffer.Allocate(AlignedSize);
    }
    else
    {
        if(m_CurrOffset == RingBuffer::InvalidOffset || AlignedSize > m_AvailableSize)
        {
            m_CurrOffset = m_ParentRingBuffer.Allocate(m_PagSize);
            m_AvailableSize = m_PagSize;
        }
        if(m_CurrOffset != RingBuffer::InvalidOffset)
        {
            Offset = m_CurrOffset;
            m_AvailableSize -= AlignedSize;
            m_CurrOffset += AlignedSize;
        }
    }

    // Every device context uses its own dynamic heap, so there is no need to lock
    if(Offset != RingBuffer::InvalidOffset)
    {
        m_CurrAllocatedSize += AlignedSize;
        m_CurrUsedSize      += SizeInBytes;
        m_PeakAllocatedSize = std::max(m_PeakAllocatedSize, m_CurrAllocatedSize);
        m_PeakUsedSize      = std::max(m_PeakUsedSize,      m_CurrUsedSize);

        return VulkanDynamicAllocation{ m_ParentRingBuffer, Offset, SizeInBytes };
    }
    else
        return VulkanDynamicAllocation{};
}

VulkanDynamicHeap::~VulkanDynamicHeap()
{
    LOG_INFO_MESSAGE(m_HeapName, " usage stats:\n"
        "    Peak used/peak allocated size: ", SizeFormatter{ m_PeakUsedSize, 2, m_PeakAllocatedSize }, '/', SizeFormatter{ m_PeakAllocatedSize, 2, m_PeakAllocatedSize },
        ". Peak utilization: ", std::fixed, std::setprecision(1), static_cast<double>(m_PeakUsedSize) / static_cast<double>(std::max(m_PeakAllocatedSize, 1U)) * 100.0, '%');
}

}

/*
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/command_container/command_encoder.h"
#include "shared/source/helpers/blit_commands_helper.h"
#include "shared/source/helpers/hw_helper.h"
#include "shared/source/helpers/timestamp_packet.h"

namespace NEO {

template <typename GfxFamily>
size_t BlitCommandsHelper<GfxFamily>::estimateBlitCommandsSize(Vec3<size_t> copySize, const CsrDependencies &csrDependencies, bool updateTimestampPacket) {
    size_t numberOfBlits = 0;
    uint64_t width = 1;
    uint64_t height = 1;

    for (uint64_t slice = 0; slice < copySize.z; slice++) {
        for (uint64_t row = 0; row < copySize.y; row++) {
            uint64_t sizeToBlit = copySize.x;
            while (sizeToBlit != 0) {
                if (sizeToBlit > BlitterConstants::maxBlitWidth) {
                    // dispatch 2D blit: maxBlitWidth x (1 .. maxBlitHeight)
                    width = BlitterConstants::maxBlitWidth;
                    height = std::min((sizeToBlit / width), BlitterConstants::maxBlitHeight);

                } else {
                    // dispatch 1D blt: (1 .. maxBlitWidth) x 1
                    width = sizeToBlit;
                    height = 1;
                }
                sizeToBlit -= (width * height);
                numberOfBlits++;
            }
        }
    }

    constexpr size_t cmdsSizePerBlit = (sizeof(typename GfxFamily::XY_COPY_BLT) + sizeof(typename GfxFamily::MI_ARB_CHECK));

    return TimestampPacketHelper::getRequiredCmdStreamSize<GfxFamily>(csrDependencies) +
           (cmdsSizePerBlit * numberOfBlits) +
           (EncodeMiFlushDW<GfxFamily>::getMiFlushDwCmdSizeForDataWrite() * static_cast<size_t>(updateTimestampPacket));
}

template <typename GfxFamily>
size_t BlitCommandsHelper<GfxFamily>::estimateBlitCommandsSize(const BlitPropertiesContainer &blitPropertiesContainer, const HardwareInfo &hwInfo) {
    size_t size = 0;
    for (auto &blitProperties : blitPropertiesContainer) {
        size += BlitCommandsHelper<GfxFamily>::estimateBlitCommandsSize(blitProperties.copySize, blitProperties.csrDependencies,
                                                                        blitProperties.outputTimestampPacket != nullptr);
    }
    size += MemorySynchronizationCommands<GfxFamily>::getSizeForAdditonalSynchronization(hwInfo);
    size += EncodeMiFlushDW<GfxFamily>::getMiFlushDwCmdSizeForDataWrite() + sizeof(typename GfxFamily::MI_BATCH_BUFFER_END);

    return alignUp(size, MemoryConstants::cacheLineSize);
}

template <typename GfxFamily>
uint64_t BlitCommandsHelper<GfxFamily>::calculateBlitCommandDestinationBaseAddress(const BlitProperties &blitProperties, uint64_t offset, uint64_t row, uint64_t slice) {
    return blitProperties.dstGpuAddress + blitProperties.dstOffset.x + offset +
           blitProperties.dstOffset.y * blitProperties.dstRowPitch +
           blitProperties.dstOffset.z * blitProperties.dstSlicePitch +
           row * blitProperties.dstRowPitch +
           slice * blitProperties.dstSlicePitch;
}

template <typename GfxFamily>
uint64_t BlitCommandsHelper<GfxFamily>::calculateBlitCommandSourceBaseAddress(const BlitProperties &blitProperties, uint64_t offset, uint64_t row, uint64_t slice) {
    return blitProperties.srcGpuAddress + blitProperties.srcOffset.x + offset +
           blitProperties.srcOffset.y * blitProperties.srcRowPitch +
           blitProperties.srcOffset.z * blitProperties.srcSlicePitch +
           row * blitProperties.srcRowPitch +
           slice * blitProperties.srcSlicePitch;
}

template <typename GfxFamily>
void BlitCommandsHelper<GfxFamily>::dispatchBlitCommandsForBuffer(const BlitProperties &blitProperties, LinearStream &linearStream, const RootDeviceEnvironment &rootDeviceEnvironment) {
    uint64_t width = 1;
    uint64_t height = 1;

    for (uint64_t slice = 0; slice < blitProperties.copySize.z; slice++) {
        for (uint64_t row = 0; row < blitProperties.copySize.y; row++) {
            uint64_t offset = 0;
            uint64_t sizeToBlit = blitProperties.copySize.x;
            while (sizeToBlit != 0) {
                if (sizeToBlit > BlitterConstants::maxBlitWidth) {
                    // dispatch 2D blit: maxBlitWidth x (1 .. maxBlitHeight)
                    width = BlitterConstants::maxBlitWidth;
                    height = std::min((sizeToBlit / width), BlitterConstants::maxBlitHeight);
                } else {
                    // dispatch 1D blt: (1 .. maxBlitWidth) x 1
                    width = sizeToBlit;
                    height = 1;
                }

                {
                    auto bltCmd = GfxFamily::cmdInitXyCopyBlt;

                    bltCmd.setTransferWidth(static_cast<uint32_t>(width));
                    bltCmd.setTransferHeight(static_cast<uint32_t>(height));
                    bltCmd.setDestinationPitch(static_cast<uint32_t>(width));
                    bltCmd.setSourcePitch(static_cast<uint32_t>(width));

                    auto dstAddr = calculateBlitCommandDestinationBaseAddress(blitProperties, offset, row, slice);
                    auto srcAddr = calculateBlitCommandSourceBaseAddress(blitProperties, offset, row, slice);

                    bltCmd.setDestinationBaseAddress(dstAddr);
                    bltCmd.setSourceBaseAddress(srcAddr);

                    appendBlitCommandsForBuffer(blitProperties, bltCmd, rootDeviceEnvironment);

                    auto bltStream = linearStream.getSpaceForCmd<typename GfxFamily::XY_COPY_BLT>();
                    *bltStream = bltCmd;
                }

                {
                    auto miArbCheckStream = linearStream.getSpaceForCmd<typename GfxFamily::MI_ARB_CHECK>();
                    *miArbCheckStream = GfxFamily::cmdInitArbCheck;
                }

                auto blitSize = width * height;
                sizeToBlit -= blitSize;
                offset += blitSize;
            }
        }
    }
}
template <typename GfxFamily>
template <size_t patternSize>
void BlitCommandsHelper<GfxFamily>::dispatchBlitMemoryFill(NEO::GraphicsAllocation *dstAlloc, uint32_t *pattern, LinearStream &linearStream, size_t size, const RootDeviceEnvironment &rootDeviceEnvironment, COLOR_DEPTH depth) {
    using XY_COLOR_BLT = typename GfxFamily::XY_COLOR_BLT;
    auto blitCmd = GfxFamily::cmdInitXyColorBlt;

    blitCmd.setFillColor(pattern);
    blitCmd.setColorDepth(depth);

    appendBlitCommandsForFillBuffer(dstAlloc, blitCmd, rootDeviceEnvironment);

    uint64_t offset = 0;
    uint64_t sizeToFill = size;
    while (sizeToFill != 0) {
        auto tmpCmd = blitCmd;
        tmpCmd.setDestinationBaseAddress(ptrOffset(dstAlloc->getGpuAddress(), static_cast<size_t>(offset)));
        uint64_t height = 0;
        uint64_t width = 0;
        if (sizeToFill <= BlitterConstants::maxBlitWidth) {
            width = sizeToFill;
            height = 1;
        } else {
            width = BlitterConstants::maxBlitWidth;
            height = std::min((sizeToFill / width), BlitterConstants::maxBlitHeight);
            if (height > 1) {
                appendTilingEnable(tmpCmd);
            }
        }
        tmpCmd.setTransferWidth(static_cast<uint32_t>(width));
        tmpCmd.setTransferHeight(static_cast<uint32_t>(height));
        tmpCmd.setDestinationPitch(static_cast<uint32_t>(width));
        auto cmd = linearStream.getSpaceForCmd<XY_COLOR_BLT>();
        *cmd = tmpCmd;
        auto blitSize = width * height;
        offset += (blitSize);
        sizeToFill -= blitSize;
    }
}

} // namespace NEO

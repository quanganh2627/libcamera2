/*
 * Copyright (C) 2013 The Android Open Source Project
 * Copyright (c) 2013 Intel Corporation. All Rights Reserved.
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

#include "LogHelper.h"
#include "MemoryUtils.h"
#include "PlatformData.h"

namespace android {
    namespace MemoryUtils {

    status_t allocateGraphicBuffer(AtomBuffer &aBuff, int width, int height)
    {
        LOG1("@%s", __FUNCTION__);
        status_t status = OK;

        MapperPointer mapperPointer;
        mapperPointer.ptr = NULL;

        int lockMode = GRALLOC_USAGE_SW_READ_OFTEN |
                    GRALLOC_USAGE_SW_WRITE_NEVER |
                    GRALLOC_USAGE_HW_COMPOSER;

        GraphicBuffer *cameraGraphicBuffer = new GraphicBuffer(width, height, getGFXHALPixelFormatFromV4L2Format(PlatformData::getPreviewFormat()),
                        GraphicBuffer::USAGE_HW_RENDER | GraphicBuffer::USAGE_SW_WRITE_OFTEN | GraphicBuffer::USAGE_HW_TEXTURE);

        if (!cameraGraphicBuffer)
            return NO_MEMORY;

        ANativeWindowBuffer *cameraNativeWindowBuffer = cameraGraphicBuffer->getNativeBuffer();
        aBuff.buff = NULL;     // We do not allocate a normal camera_memory_t
        aBuff.width = width;
        aBuff.height = height;
        aBuff.stride = cameraNativeWindowBuffer->stride;
        aBuff.format = PlatformData::getPreviewFormat();
        aBuff.gfxInfo.scalerId = -1;
        aBuff.gfxInfo.gfxBufferHandle = &cameraGraphicBuffer->handle;
        aBuff.gfxInfo.gfxBuffer = cameraGraphicBuffer;
        cameraGraphicBuffer->incStrong(&aBuff);
        aBuff.size = frameSize(V4L2_PIX_FMT_NV12, aBuff.stride, aBuff.height);

        status = cameraGraphicBuffer->lock(lockMode, &mapperPointer.ptr);
        aBuff.gfxInfo.locked = true;
        if (status != NO_ERROR) {
            LOGE("@%s: Failed to lock GraphicBuffer!", __FUNCTION__);
            return UNKNOWN_ERROR;
        }

        aBuff.dataPtr = mapperPointer.ptr;
        aBuff.shared = false;
        LOG1("@%s allocated gfx buffer with pointer %p nativewindowbuf %p",
            __FUNCTION__, aBuff.dataPtr, cameraNativeWindowBuffer);
        return status;
    }

    void freeGraphicBuffer(AtomBuffer &aBuff)
    {
        LOG1("@%s", __FUNCTION__);
        GraphicBuffer *graphicBuffer = aBuff.gfxInfo.gfxBuffer;
        if (graphicBuffer) { // if gfx buffers came through setGraphicPreviewBuffers, there is no graphic buffer stored..
            LOG1("@%s freeing gfx buffer with pointer %p (graphic win buf %p) refcount %d",
                __FUNCTION__, aBuff.dataPtr, graphicBuffer, graphicBuffer->getStrongCount());
            if (aBuff.gfxInfo.locked)
                graphicBuffer->unlock();

            graphicBuffer->decStrong(&aBuff);
        }
        aBuff.gfxInfo.gfxBuffer = NULL;
        aBuff.gfxInfo.gfxBufferHandle = NULL;
        aBuff.gfxInfo.scalerId = -1;
        aBuff.gfxInfo.locked = false;
        aBuff.dataPtr = NULL;
    }

    void freeAtomBuffer(AtomBuffer &aBuff)
    {
        LOG1("@%s", __FUNCTION__);
        // free GFX memory, if any
        freeGraphicBuffer(aBuff);
        // free memory allocated through callbacks, if any
        if (aBuff.buff != NULL) {
            aBuff.buff->release(aBuff.buff);
            aBuff.buff = NULL;
        }
        // free metadata, if any
        if (aBuff.metadata_buff != NULL) {
            aBuff.metadata_buff->release(aBuff.metadata_buff);
            aBuff.metadata_buff = NULL;
        }
        aBuff.dataPtr = NULL;
    }

    } // namespace MemoryUtils
} // namespace android
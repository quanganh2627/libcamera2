/*
 * Copyright (C) 2011 The Android Open Source Project
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

#define LOG_TAG "Camera_Callbacks"

#include "LogHelper.h"
#include "Callbacks.h"
#include "intel_camera_extensions.h"
#include "PerformanceTraces.h"
#include "cutils/atomic.h"
#include "PlatformData.h"

namespace android {

Callbacks* Callbacks::mInstance = NULL;

Callbacks::Callbacks() :
    mNotifyCB(NULL)
    ,mDataCB(NULL)
    ,mDataCBTimestamp(NULL)
    ,mGetMemoryCB(NULL)
    ,mUserToken(NULL)
    ,mMessageFlags(0)
    ,mDummyByte(NULL)
    ,mPanoramaMetadata(NULL)
    ,mStoreMetaDataInBuffers(false)
{
    LOG1("@%s", __FUNCTION__);
}

Callbacks::~Callbacks()
{
    LOG1("@%s", __FUNCTION__);
    mInstance = NULL;
    if (mDummyByte != NULL) {
        mDummyByte->release(mDummyByte);
        mDummyByte = NULL;
    }
    if (mPanoramaMetadata != NULL) {
        mPanoramaMetadata->release(mPanoramaMetadata);
        mPanoramaMetadata = NULL;
    }
}

void Callbacks::setCallbacks(camera_notify_callback notify_cb,
                             camera_data_callback data_cb,
                             camera_data_timestamp_callback data_cb_timestamp,
                             camera_request_memory get_memory,
                             void* user)
{
    LOG1("@%s: Notify = %p, Data = %p, DataTimestamp = %p, GetMemory = %p",
            __FUNCTION__,
            notify_cb,
            data_cb,
            data_cb_timestamp,
            get_memory);
    mNotifyCB = notify_cb;
    mDataCB = data_cb;
    mDataCBTimestamp = data_cb_timestamp;
    mGetMemoryCB = get_memory;
    mUserToken = user;
}

void Callbacks::enableMsgType(int32_t msgType)
{
    LOG1("@%s: msgType = 0x%08x", __FUNCTION__, msgType);
    android_atomic_or(msgType, (int32_t*)&mMessageFlags);
    LOG1("@%s: mMessageFlags = 0x%08x", __FUNCTION__,  mMessageFlags);
}

void Callbacks::disableMsgType(int32_t msgType)
{
    LOG1("@%s: msgType = 0x%08x", __FUNCTION__, msgType);
    android_atomic_and(~msgType, (int32_t*)&mMessageFlags);
    LOG1("@%s: mMessageFlags = 0x%08x", __FUNCTION__,  mMessageFlags);
}

bool Callbacks::msgTypeEnabled(int32_t msgType)
{
    return (mMessageFlags & msgType) != 0;
}

void Callbacks::panoramaSnapshot(const AtomBuffer &livePreview)
{
    LOG2("@%s", __FUNCTION__);
    mDataCB(CAMERA_MSG_PANORAMA_SNAPSHOT, livePreview.buff, 0, NULL, mUserToken);
}

void Callbacks::panoramaDisplUpdate(camera_panorama_metadata &metadata)
{
    LOG2("@%s", __FUNCTION__);
    if (mPanoramaMetadata == NULL)
        mPanoramaMetadata = mGetMemoryCB(-1, sizeof(camera_panorama_metadata), 1, NULL);
    memcpy(mPanoramaMetadata->data, &metadata, sizeof(camera_panorama_metadata));
    mDataCB(CAMERA_MSG_PANORAMA_METADATA, mPanoramaMetadata, 0, NULL, mUserToken);
}

void Callbacks::previewFrameDone(AtomBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_PREVIEW_FRAME) && mDataCB != NULL) {
        LOG2("Sending message: CAMERA_MSG_PREVIEW_FRAME, buff id = %d, size = %zu", buff->id,  buff->buff->size);
        mDataCB(CAMERA_MSG_PREVIEW_FRAME, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::videoFrameDone(AtomBuffer *buff, nsecs_t timestamp)
{
    LOG2("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_VIDEO_FRAME) && mDataCBTimestamp != NULL) {
        LOG2("Sending message: CAMERA_MSG_VIDEO_FRAME, buff id = %d", buff->id);
        if(mStoreMetaDataInBuffers) {
            mDataCBTimestamp(timestamp, CAMERA_MSG_VIDEO_FRAME, buff->metadata_buff, 0, mUserToken);
        } else {
            mDataCBTimestamp(timestamp, CAMERA_MSG_VIDEO_FRAME, buff->buff, 0, mUserToken);
        }
    }
}

void Callbacks::compressedFrameDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_COMPRESSED_IMAGE) && mDataCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_COMPRESSED_IMAGE, buff id = %d, size = %zu", buff->id, buff->buff->size);
        mDataCB(CAMERA_MSG_COMPRESSED_IMAGE, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::postviewFrameDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_POSTVIEW_FRAME) && mDataCB != NULL) {
        LOGD("Sending message: CAMERA_MSG_POSTVIEW_FRAME, buff id = %d, size = %zu", buff->id,  buff->buff->size);
        mDataCB(CAMERA_MSG_POSTVIEW_FRAME, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::rawFrameDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_RAW_IMAGE_NOTIFY) && mNotifyCB != NULL) {
        LOGD("Sending message: CAMERA_MSG_RAW_IMAGE_NOTIFY, buff id = %d", buff->id);
        mNotifyCB(CAMERA_MSG_RAW_IMAGE_NOTIFY, 0, 0, mUserToken);
    }

    if ((mMessageFlags & CAMERA_MSG_RAW_IMAGE) && mNotifyCB != NULL) {
        LOGD("Sending message: CAMERA_MSG_RAW_IMAGE, buff id = %d, size = %zu", buff->id, buff->buff->size);
        mDataCB(CAMERA_MSG_RAW_IMAGE, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::cameraError(int err)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_ERROR) && mNotifyCB != NULL) {
        LOGD("Sending message: CAMERA_MSG_ERROR, err # = %d", err);
        mNotifyCB(CAMERA_MSG_ERROR, err, 0, mUserToken);
    }
}

void Callbacks::facesDetected(camera_frame_metadata_t &face_metadata)
{
 /*If the Call back is enabled for meta data and face detection is
    * active, inform about faces.*/
    if ((mMessageFlags & CAMERA_MSG_PREVIEW_METADATA)){
        // We can't pass NULL to camera service, otherwise it
        // will handle it as notification callback. So we need a dummy.
        // Another bad design from Google.
        if (mDummyByte == NULL) mDummyByte = mGetMemoryCB(-1, 1, 1, NULL);
        mDataCB(CAMERA_MSG_PREVIEW_METADATA,
             mDummyByte,
             0,
             &face_metadata,
             mUserToken);
    }
}

void Callbacks::sceneDetected(int sceneMode, bool sceneHdr)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_SCENE_DETECT) && mNotifyCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_SCENE_DETECT, scene = %d, HDR = %d", sceneMode, (int) sceneHdr);
        mNotifyCB(CAMERA_MSG_SCENE_DETECT, sceneMode, (int) sceneHdr, mUserToken);
    }
}

status_t Callbacks::allocateGraphicBuffer(AtomBuffer &buff, int width, int height)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = OK;

    MapperPointer mapperPointer;
    mapperPointer.ptr = NULL;

    int lockMode = GRALLOC_USAGE_SW_READ_OFTEN |
                   GRALLOC_USAGE_SW_WRITE_NEVER |
                   GRALLOC_USAGE_HW_COMPOSER;

    GraphicBuffer *cameraGraphicBuffer = new GraphicBuffer(width, height, PlatformData::getGFXHALPixelFormat(),
                    GraphicBuffer::USAGE_HW_RENDER | GraphicBuffer::USAGE_SW_WRITE_OFTEN | GraphicBuffer::USAGE_HW_TEXTURE);

    if (!cameraGraphicBuffer)
        return NO_MEMORY;

    ANativeWindowBuffer *cameraNativeWindowBuffer = cameraGraphicBuffer->getNativeBuffer();
    buff.buff = NULL;     // We do not allocate a normal camera_memory_t
    buff.width = width;
    buff.height = height;
    buff.stride = cameraNativeWindowBuffer->stride;
    buff.format = PlatformData::getPreviewFormat();
    buff.gfxInfo.scalerId = -1;
    buff.gfxInfo.gfxBufferHandle = &cameraGraphicBuffer->handle;
    buff.gfxInfo.gfxBuffer = cameraGraphicBuffer;
    cameraGraphicBuffer->incStrong(this);
    buff.size = frameSize(V4L2_PIX_FMT_NV12, buff.stride, buff.height);

    status = cameraGraphicBuffer->lock(lockMode, &mapperPointer.ptr);
    buff.gfxInfo.locked = true;
    if (status != NO_ERROR) {
        LOGE("@%s: Failed to lock GraphicBuffer!", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    buff.dataPtr = mapperPointer.ptr;
    buff.shared = false;
    LOG1("@%s allocated gfx buffer with pointer %p nativewindowbuf %p", __FUNCTION__, buff.dataPtr, cameraNativeWindowBuffer);
    return status;
}

void Callbacks::allocateMemory(AtomBuffer *buff, int size, bool cached)
{
    LOG1("@%s: size %d", __FUNCTION__, size);
    buff->buff = NULL;
    if (mGetMemoryCB != NULL) {
        /*
         * Because using uncached memory saves more power for video encoder
         * during video recording, so the function provides the API to allocate
         * cached/uncached memory, the parameter "fd" in the function "mGetMemoryCB()"
         * will be used for that.
         * "-1" means "cached memory"
         * "-2" means "uncached memory"
         */
        if(cached)
            buff->buff = mGetMemoryCB(-1, size, 1, mUserToken);
        else
            buff->buff = mGetMemoryCB(-2, size, 1, mUserToken);

        if (buff->buff != NULL) {
            buff->dataPtr = buff->buff->data;
            buff->size = buff->buff->size;
        } else {
            LOGE("Memory allocation failed (get memory callback return null)");
            buff->dataPtr = NULL;
            buff->size = 0;
        }
    } else {
        LOGE("Memory allocation failed (missing get memory callback)");
        buff->buff = NULL;
        buff->dataPtr = NULL;
        buff->size = 0;
    }
}

void Callbacks::allocateMemory(camera_memory_t **buff, size_t size, bool cached)
{
    LOG1("@%s", __FUNCTION__);
    *buff = NULL;
    if (mGetMemoryCB != NULL) {
        if(cached)
            *buff = mGetMemoryCB(-1, size, 1, mUserToken);
        else
            *buff = mGetMemoryCB(-2, size, 1, mUserToken);
    }
}

void Callbacks::autofocusDone(bool status)
{
    LOG1("@%s", __FUNCTION__);
    if (mMessageFlags & CAMERA_MSG_FOCUS) {
        LOG1("Sending message: CAMERA_MSG_FOCUS");
        mNotifyCB(CAMERA_MSG_FOCUS, status, 0, mUserToken);
    }
}

void Callbacks::focusMove(bool start)
{
    LOG1("@%s", __FUNCTION__);
    if (mMessageFlags & CAMERA_MSG_FOCUS_MOVE) {
        LOG2("Sending message: CAMERA_MSG_FOCUS_MOVE");
        mNotifyCB(CAMERA_MSG_FOCUS_MOVE, start, 0, mUserToken);
    }
}

void Callbacks::shutterSound()
{
    LOG1("@%s", __FUNCTION__);
    if (mMessageFlags & CAMERA_MSG_SHUTTER) {
        LOG1("Sending message: CAMERA_MSG_SHUTTER");
        mNotifyCB(CAMERA_MSG_SHUTTER, 1, 0, mUserToken);
    }
}

status_t Callbacks::storeMetaDataInBuffers(bool enabled)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    mStoreMetaDataInBuffers = enabled;
    return status;
}

void Callbacks::ullPictureDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if (mDataCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_ULL_SNAPSHOT, buff id = %d, size = %zu", buff->id, buff->buff->size);
        mDataCB(CAMERA_MSG_ULL_SNAPSHOT, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::ullTriggered(int id)
{
    LOG1("@%s", __FUNCTION__);

    if (mNotifyCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_ULL_TRIGGERED, id = %d", id);
        mNotifyCB(CAMERA_MSG_ULL_TRIGGERED, id, 0, mUserToken);
    }

}

};

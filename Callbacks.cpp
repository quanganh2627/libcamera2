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
namespace android {

Callbacks* Callbacks::mInstance = NULL;

Callbacks::Callbacks() :
    mNotifyCB(NULL)
    ,mDataCB(NULL)
    ,mDataCBTimestamp(NULL)
    ,mGetMemoryCB(NULL)
    ,mUserToken(NULL)
    ,mDummyByte(NULL)
{
    LOG1("@%s", __FUNCTION__);
}

Callbacks::~Callbacks()
{
    LOG1("@%s", __FUNCTION__);
    mInstance = NULL;
    if (mDummyByte != NULL) mDummyByte->release(mDummyByte);
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
    LOG1("@%s: msgType = %d", __FUNCTION__, msgType);
    mMessageFlags |= msgType;
}

void Callbacks::disableMsgType(int32_t msgType)
{
    LOG1("@%s: msgType = %d", __FUNCTION__, msgType);
    mMessageFlags &= ~msgType;
}

bool Callbacks::msgTypeEnabled(int32_t msgType)
{
    return (mMessageFlags & msgType) != 0;
}

void Callbacks::previewFrameDone(AtomBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_PREVIEW_FRAME) && mDataCB != NULL) {
        LOG2("Sending message: CAMERA_MSG_PREVIEW_FRAME, buff id = %d", buff->id);
        mDataCB(CAMERA_MSG_PREVIEW_FRAME, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::videoFrameDone(AtomBuffer *buff, nsecs_t timestamp)
{
    LOG2("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_VIDEO_FRAME) && mDataCBTimestamp != NULL) {
        LOG2("Sending message: CAMERA_MSG_VIDEO_FRAME, buff id = %d", buff->id);
        mDataCBTimestamp(timestamp, CAMERA_MSG_VIDEO_FRAME, buff->buff, 0, mUserToken);
    }
}

void Callbacks::compressedFrameDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_COMPRESSED_IMAGE) && mDataCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_COMPRESSED_IMAGE, buff id = %d", buff->id);
        mDataCB(CAMERA_MSG_COMPRESSED_IMAGE, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::postviewFrameDone(AtomBuffer *buff)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_POSTVIEW_FRAME) && mDataCB != NULL) {
        LOGD("Sending message: CAMERA_MSG_POSTVIEW_FRAME, buff id = %d", buff->id);
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
        LOGD("Sending message: CAMERA_MSG_RAW_IMAGE, buff id = %d", buff->id);
        mDataCB(CAMERA_MSG_RAW_IMAGE, buff->buff, 0, NULL, mUserToken);
    }
}

void Callbacks::cameraError(int err)
{
    LOG1("@%s", __FUNCTION__);
    if ((mMessageFlags & CAMERA_MSG_ERROR) && mNotifyCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_ERROR, err # = %d", err);
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
#if 0
    if ((mMessageFlags & CAMERA_MSG_SCENE_DETECT) && mNotifyCB != NULL) {
        LOG1("Sending message: CAMERA_MSG_SCENE_DETECT, scene = %d, HDR = %d", sceneMode, (int) sceneHdr);
        mNotifyCB(CAMERA_MSG_SCENE_DETECT, sceneMode, (int) sceneHdr, mUserToken);
    }
#endif
}

void Callbacks::allocateMemory(AtomBuffer *buff, int size)
{
    LOG1("@%s", __FUNCTION__);
    buff->buff = NULL;
    if (mGetMemoryCB != NULL)
        buff->buff = mGetMemoryCB(-1, size, 1, mUserToken);
}

void Callbacks::autofocusDone(bool status)
{
    LOG1("@%s", __FUNCTION__);
    if (mMessageFlags & CAMERA_MSG_FOCUS) {
        LOG1("Sending message: CAMERA_MSG_FOCUS");
        mNotifyCB(CAMERA_MSG_FOCUS, status, 0, mUserToken);
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

};

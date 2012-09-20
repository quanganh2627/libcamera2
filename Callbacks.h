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

#ifndef ANDROID_LIBCAMERA_CALLBACKS
#define ANDROID_LIBCAMERA_CALLBACKS

#include "intel_camera_extensions.h"
#include <camera.h>
#include <utils/threads.h>
#include <utils/Timers.h>
#include "AtomCommon.h"
namespace android {

class Callbacks {

    static Callbacks* mInstance;
    Callbacks();
public:
    static Callbacks* getInstance() {
        if (mInstance == NULL) {
            mInstance = new Callbacks();
        }
        return mInstance;
    }
    virtual ~Callbacks();

public:

    void setCallbacks(camera_notify_callback notify_cb,
                      camera_data_callback data_cb,
                      camera_data_timestamp_callback data_cb_timestamp,
                      camera_request_memory get_memory,
                      void* user);

    void enableMsgType(int32_t msgType);
    void disableMsgType(int32_t msgType);
    bool msgTypeEnabled(int32_t msgType);

    void previewFrameDone(AtomBuffer *buff);
    void videoFrameDone(AtomBuffer *buff, nsecs_t timstamp);
    void compressedFrameDone(AtomBuffer *buff);
    void postviewFrameDone(AtomBuffer *buff);
    void rawFrameDone(AtomBuffer *buff);
    void cameraError(int err);
    void autofocusDone(bool status);
    void focusMove(bool start);
    void shutterSound();

    void allocateMemory(AtomBuffer *buff, int size);
    void allocateMemory(camera_memory_t **buff, int size);
    void facesDetected(camera_frame_metadata_t &face_metadata);
    void sceneDetected(int sceneMode, bool sceneHdr);
    void panoramaDisplUpdate(camera_panorama_metadata &metadata);
    void panoramaSnapshot(AtomBuffer &livePreview);
    status_t storeMetaDataInBuffers(bool enabled);

private:
    camera_notify_callback mNotifyCB;
    camera_data_callback mDataCB;
    camera_data_timestamp_callback mDataCBTimestamp;
    camera_request_memory mGetMemoryCB;
    void *mUserToken;
    uint32_t mMessageFlags;
    camera_memory_t* mDummyByte;
    camera_memory_t* mPanoramaMetadata;
    bool mStoreMetaDataInBuffers;
};

};

#endif  // ANDROID_LIBCAMERA_CALLBACKS

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
#define LOG_TAG "Atom_ControlThread"

#include "ControlThread.h"
#include "LogHelper.h"
#include "PreviewThread.h"
#include "PictureThread.h"
#include "AtomISP.h"
#include "Callbacks.h"
#include "CallbacksThread.h"
#include "ColorConverter.h"
#include "IntelBufferSharing.h"
#include "FaceDetectorFactory.h"
#include <utils/Vector.h>
#include <math.h>

namespace android {

/*
 * NUM_WARMUP_FRAMES: used for front camera only
 * Since front camera does not 3A, it actually has 2A (auto-exposure and auto-whitebalance),
 * it needs about 4 for internal 2A from driver to gather enough information and establish
 * the correct values for 2A.
 */
#define NUM_WARMUP_FRAMES 4
/*
 * NUM_BURST_BUFFERS: used for burst captures
 */
#define NUM_BURST_BUFFERS 10
/*
 * MAX_JPEG_BUFFERS: the maximum numbers of queued JPEG buffers
 */
#define MAX_JPEG_BUFFERS 4

/*
 * ASPECT_TOLERANCE: the tolerance between aspect ratios to consider them the same
 */
#define ASPECT_TOLERANCE 0.001

ControlThread::ControlThread(int cameraId) :
    Thread(true) // callbacks may call into java
    ,mISP(new AtomISP(cameraId))
    ,mAAA(AtomAAA::getInstance())
    ,mPreviewThread(new PreviewThread((ICallbackPreview *) this))
    ,mPictureThread(new PictureThread((ICallbackPicture *) this))
    ,mVideoThread(new VideoThread())
    ,m3AThread(new AAAThread((ICallbackAAA *) this))
    ,mMessageQueue("ControlThread", (int) MESSAGE_ID_MAX)
    ,mState(STATE_STOPPED)
    ,mThreadRunning(false)
    ,mCallbacks(Callbacks::getInstance())
    ,mCallbacksThread(CallbacksThread::getInstance())
    ,mCoupledBuffers(NULL)
    ,mNumBuffers(mISP->getNumBuffers())
    ,m_pFaceDetector(0)
    ,mFaceDetectionActive(false)
    ,mFlashNeeded(false)
    ,mBSInstance(BufferShareRegistry::getInstance())
    ,mBSState(BS_STATE_DISABLED)
    ,mLastRecordingBuffIndex(0)
{
    LOG1("@%s: cameraId = %d", __FUNCTION__, cameraId);

    // get default params from AtomISP and JPEG encoder
    mISP->getDefaultParameters(&mParameters);
    mPictureThread->getDefaultParameters(&mParameters);
    mPreviewThread->getDefaultParameters(&mParameters);

    status_t status = m3AThread->run();
    if (status != NO_ERROR) {
        LOGE("Error starting 3A thread!");
    }
    status = mPreviewThread->run();
    if (status != NO_ERROR) {
        LOGE("Error starting preview thread!");
    }
    status = mPictureThread->run();
    if (status != NO_ERROR) {
        LOGW("Error starting picture thread!");
    }
    status = mCallbacksThread->run();
    if (status != NO_ERROR) {
        LOGW("Error starting callbacks thread!");
    }
    status = mVideoThread->run();
    if (status != NO_ERROR) {
        LOGW("Error starting video thread!");
    }
    m_pFaceDetector=FaceDetectorFactory::createDetector(mCallbacks);
    if (m_pFaceDetector != 0){
        mParameters.set(CameraParameters::KEY_MAX_NUM_DETECTED_FACES_HW,
                m_pFaceDetector->getMaxFacesDetectable());
    } else {
        LOGE("Failed on creating face detector.");
    }
}

ControlThread::~ControlThread()
{
    LOG1("@%s", __FUNCTION__);

    mPreviewThread->requestExitAndWait();
    mPreviewThread.clear();

    mPictureThread->requestExitAndWait();
    mPictureThread.clear();

    mCallbacksThread->requestExitAndWait();
    mCallbacksThread.clear();

    mVideoThread->requestExitAndWait();
    mVideoThread.clear();

    m3AThread->requestExitAndWait();
    m3AThread.clear();

    mBSInstance.clear();
    if (mISP != NULL) {
        delete mISP;
    }
    if (mAAA != NULL) {
        delete mAAA;
    }
    if (mCallbacks != NULL) {
        delete mCallbacks;
    }
    if (m_pFaceDetector != 0) {
        if (!FaceDetectorFactory::destroyDetector(m_pFaceDetector)){
            LOGE("Failed on destroy face detector thru factory");
            delete m_pFaceDetector;//should not happen.
        }
        m_pFaceDetector = 0;
    }
}

status_t ControlThread::setPreviewWindow(struct preview_stream_ops *window)
{
    LOG1("@%s: window = %p", __FUNCTION__, window);
    if (mPreviewThread != NULL) {
        return mPreviewThread->setPreviewWindow(window);
    }
    return NO_ERROR;
}

void ControlThread::setCallbacks(camera_notify_callback notify_cb,
                                 camera_data_callback data_cb,
                                 camera_data_timestamp_callback data_cb_timestamp,
                                 camera_request_memory get_memory,
                                 void* user)
{
    LOG1("@%s", __FUNCTION__);
    mCallbacks->setCallbacks(notify_cb,
            data_cb,
            data_cb_timestamp,
            get_memory,
            user);
}

void ControlThread::enableMsgType(int32_t msgType)
{
    LOG2("@%s", __FUNCTION__);
    mCallbacks->enableMsgType(msgType);
}

void ControlThread::disableMsgType(int32_t msgType)
{
    LOG2("@%s", __FUNCTION__);
    mCallbacks->disableMsgType(msgType);
}

bool ControlThread::msgTypeEnabled(int32_t msgType)
{
    LOG2("@%s", __FUNCTION__);
    return mCallbacks->msgTypeEnabled(msgType);
}

status_t ControlThread::startPreview()
{
    LOG1("@%s", __FUNCTION__);
    // send message and block until thread processes message
    Message msg;
    msg.id = MESSAGE_ID_START_PREVIEW;
    return mMessageQueue.send(&msg, MESSAGE_ID_START_PREVIEW);
}

status_t ControlThread::stopPreview()
{
    LOG1("@%s", __FUNCTION__);
    // send message and block until thread processes message
    if (mState == STATE_STOPPED) {
        return NO_ERROR;
    }

    Message msg;
    msg.id = MESSAGE_ID_STOP_PREVIEW;
    return mMessageQueue.send(&msg, MESSAGE_ID_STOP_PREVIEW);
}

status_t ControlThread::startRecording()
{
    LOG1("@%s", __FUNCTION__);
    // send message and block until thread processes message
    Message msg;
    msg.id = MESSAGE_ID_START_RECORDING;
    return mMessageQueue.send(&msg, MESSAGE_ID_START_RECORDING);
}

status_t ControlThread::stopRecording()
{
    LOG1("@%s", __FUNCTION__);
    // send message and block until thread processes message
    Message msg;
    msg.id = MESSAGE_ID_STOP_RECORDING;
    return mMessageQueue.send(&msg, MESSAGE_ID_STOP_RECORDING);
}

bool ControlThread::previewEnabled()
{
    LOG2("@%s", __FUNCTION__);
    bool enabled = (mState == STATE_PREVIEW_STILL ||
            mState == STATE_PREVIEW_VIDEO ||
            mState == STATE_RECORDING);
    return enabled;
}

bool ControlThread::recordingEnabled()
{
    LOG2("@%s", __FUNCTION__);
    return mState == STATE_RECORDING;
}

status_t ControlThread::setParameters(const char *params)
{
    LOG1("@%s: params = %p", __FUNCTION__, params);
    Message msg;
    msg.id = MESSAGE_ID_SET_PARAMETERS;
    msg.data.setParameters.params = const_cast<char*>(params); // We swear we won't modify params :)
    return mMessageQueue.send(&msg, MESSAGE_ID_SET_PARAMETERS);
}

char* ControlThread::getParameters()
{
    LOG1("@%s", __FUNCTION__);

    char *params = NULL;
    Message msg;
    msg.id = MESSAGE_ID_GET_PARAMETERS;
    msg.data.getParameters.params = &params; // let control thread allocate and set pointer
    mMessageQueue.send(&msg, MESSAGE_ID_GET_PARAMETERS);
    return params;
}

void ControlThread::putParameters(char* params)
{
    LOG1("@%s: params = %p", __FUNCTION__, params);
    if (params)
        free(params);
}

bool ControlThread::isParameterSet(const char* param)
{
    const char* strParam = mParameters.get(param);
    int len = strlen(CameraParameters::TRUE);
    if (strParam != NULL && strncmp(strParam, CameraParameters::TRUE, len) == 0) {
        return true;
    }
    return false;
}

status_t ControlThread::takePicture()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_TAKE_PICTURE;
    return mMessageQueue.send(&msg);
}

status_t ControlThread::cancelPicture()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_CANCEL_PICTURE;
    return mMessageQueue.send(&msg);
}

status_t ControlThread::autoFocus()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_AUTO_FOCUS;
    return mMessageQueue.send(&msg);
}

status_t ControlThread::cancelAutoFocus()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_CANCEL_AUTO_FOCUS;
    return mMessageQueue.send(&msg);
}

status_t ControlThread::releaseRecordingFrame(void *buff)
{
    LOG2("@%s: buff = %p", __FUNCTION__, buff);
    Message msg;
    msg.id = MESSAGE_ID_RELEASE_RECORDING_FRAME;
    msg.data.releaseRecordingFrame.buff = buff;
    return mMessageQueue.send(&msg);
}

void ControlThread::previewDone(AtomBuffer *buff)
{
    LOG2("@%s: buff = %p, id = %d", __FUNCTION__, buff->buff->data, buff->id);
    Message msg;
    msg.id = MESSAGE_ID_PREVIEW_DONE;
    msg.data.previewDone.buff = *buff;
    mMessageQueue.send(&msg);
}
void ControlThread::returnBuffer(AtomBuffer *buff)
{
    LOG2("@%s: buff = %p, id = %d", __FUNCTION__, buff->buff->data, buff->id);
    if (buff->type == MODE_PREVIEW) {
        buff->owner = 0;
        releasePreviewFrame (buff);
    }
}
void ControlThread::releasePreviewFrame(AtomBuffer *buff)
{
    LOG2("release preview frame buffer data %p, id = %d", buff->buff->data, buff->id);
    Message msg;
    msg.id = MESSAGE_ID_RELEASE_PREVIEW_FRAME;
    msg.data.releasePreviewFrame.buff = *buff;
    mMessageQueue.send(&msg);
}

void ControlThread::pictureDone(AtomBuffer *snapshotBuf, AtomBuffer *postviewBuf)
{
    LOG2("@%s: snapshotBuf = %p, postviewBuf = %p, id = %d",
            __FUNCTION__,
            snapshotBuf->buff->data,
            postviewBuf->buff->data,
            snapshotBuf->id);
    Message msg;
    msg.id = MESSAGE_ID_PICTURE_DONE;
    msg.data.pictureDone.snapshotBuf = *snapshotBuf;
    msg.data.pictureDone.postviewBuf = *postviewBuf;
    mMessageQueue.send(&msg);
}

void ControlThread::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
    Message msg;
    msg.id = MESSAGE_ID_COMMAND;
    msg.data.command.cmd_id = cmd;
    msg.data.command.arg1 = arg1;
    msg.data.command.arg2 = arg2;
    mMessageQueue.send(&msg);
}

void ControlThread::redEyeRemovalDone(AtomBuffer *snapshotBuf, AtomBuffer *postviewBuf)
{
    LOG1("@%s: snapshotBuf = %p, postviewBuf = %p, id = %d",
            __FUNCTION__,
            snapshotBuf->buff->data,
            postviewBuf->buff->data,
            snapshotBuf->id);
    Message msg;
    msg.id = MESSAGE_ID_REDEYE_REMOVAL_DONE;
    msg.data.redEyeRemovalDone.snapshotBuf = *snapshotBuf;
    if (postviewBuf)
        msg.data.redEyeRemovalDone.postviewBuf = *postviewBuf;
    else
        msg.data.redEyeRemovalDone.postviewBuf.buff->data = NULL; // optional
    mMessageQueue.send(&msg);
}

void ControlThread::autoFocusDone()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_AUTO_FOCUS_DONE;
    mMessageQueue.send(&msg);
}

status_t ControlThread::handleMessageExit()
{
    LOG1("@%s", __FUNCTION__);
    mThreadRunning = false;

    // TODO: any other cleanup that may need to be done

    return NO_ERROR;
}

status_t ControlThread::startPreviewCore(bool videoMode)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int width;
    int height;
    int format;
    State state;
    AtomMode mode;

    if (mState != STATE_STOPPED) {
        LOGE("Must be in STATE_STOPPED to start preview");
        return INVALID_OPERATION;
    }

    if (videoMode) {
        LOG1("Starting preview in video mode");
        state = STATE_PREVIEW_VIDEO;
        mode = MODE_VIDEO;
    } else {
        LOG1("Starting preview in still mode");
        state = STATE_PREVIEW_STILL;
        mode = MODE_PREVIEW;
    }

    // set preview frame config
    format = V4L2Format(mParameters.getPreviewFormat());
    if (format == -1) {
        LOGE("Bad preview format. Cannot start the preview!");
        return BAD_VALUE;
    }
    LOG1("Using preview format: %s", v4l2Fmt2Str(format));
    mParameters.getPreviewSize(&width, &height);
    mISP->setPreviewFrameFormat(width, height);
    mPreviewThread->setPreviewConfig(width, height, format);

    // set video frame config
    if (videoMode) {
        mParameters.getVideoSize(&width, &height);
        mISP->setVideoFrameFormat(width, height);
    }

    mNumBuffers = mISP->getNumBuffers();
    mCoupledBuffers = new CoupledBuffer[mNumBuffers];
    memset(mCoupledBuffers, 0, mNumBuffers * sizeof(CoupledBuffer));

    // start the data flow
    status = mISP->start(mode);
    if (status == NO_ERROR) {
        memset(mCoupledBuffers, 0, sizeof(mCoupledBuffers));
        mState = state;
        if (mAAA->is3ASupported()) {
            // Enable auto-focus by default
            mAAA->setAfEnabled(true);
            m3AThread->enable3A();
            if (videoMode) {
                m3AThread->enableDVS(true);
            }
        }
    } else {
        LOGE("Error starting ISP!");
    }

    return status;
}

status_t ControlThread::stopPreviewCore()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    status = mPreviewThread->flushMessages();
    if (mState == STATE_PREVIEW_VIDEO && mAAA->is3ASupported()) {
        m3AThread->enableDVS(false);
    }
    status = mISP->stop();
    if (status == NO_ERROR) {
        mState = STATE_STOPPED;
    } else {
        LOGE("Error stopping ISP in preview mode!");
    }
    delete [] mCoupledBuffers;
    // set to null because frames can be returned to hal in stop state
    // need to check for null in relevant locations
    mCoupledBuffers = NULL;
    return status;
}

status_t ControlThread::stopCapture()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mState != STATE_CAPTURE) {
        LOGE("Must be in STATE_CAPTURE to stop capture");
        return INVALID_OPERATION;
    }

    status = mPictureThread->flushMessages();
    if (status != NO_ERROR) {
        LOGE("Error flushing PictureThread!");
        return status;
    }

    status = mISP->stop();
    if (status != NO_ERROR) {
        LOGE("Error stopping ISP!");
        return status;
    }

    mState = STATE_STOPPED;
    return status;
}

status_t ControlThread::restartPreview(bool videoMode)
{
    LOG1("@%s: mode = %s", __FUNCTION__, videoMode?"VIDEO":"STILL");
    bool faceActive = mFaceDetectionActive;
    stopFaceDetection(true);
    status_t status = stopPreviewCore();
    if (status == NO_ERROR)
        status = startPreviewCore(videoMode);
    if (faceActive)
        startFaceDetection();
    return status;
}

status_t ControlThread::handleMessageStartPreview()
{
    LOG1("@%s", __FUNCTION__);
    status_t status;
    if (mState == STATE_CAPTURE) {
        status = stopCapture();
        if (status != NO_ERROR) {
            LOGE("Could not stop capture before start preview!");
            return status;
        }
    }
    if (mState == STATE_STOPPED) {
        // API says apps should call startFaceDetection when resuming preview
        // stop FD here to avoid accidental FD.
        stopFaceDetection();
        bool videoMode = isParameterSet(CameraParameters::KEY_RECORDING_HINT) ? true : false;
        status = startPreviewCore(videoMode);
    } else {
        LOGE("Error starting preview. Invalid state!");
        status = INVALID_OPERATION;
    }

    // return status and unblock message sender
    mMessageQueue.reply(MESSAGE_ID_START_PREVIEW, status);
    return status;
}

status_t ControlThread::handleMessageStopPreview()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    // In STATE_CAPTURE, preview is already stopped, nothing to do
    if (mState != STATE_CAPTURE) {
        stopFaceDetection(true);
        if (mState != STATE_STOPPED) {
            status = stopPreviewCore();
        } else {
            LOGE("Error stopping preview. Invalid state!");
            status = INVALID_OPERATION;
        }
    }
    // return status and unblock message sender
    mMessageQueue.reply(MESSAGE_ID_STOP_PREVIEW, status);
    return status;
}

status_t ControlThread::handleMessageStartRecording()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    FlashMode flashMode = CAM_AE_FLASH_MODE_NOT_SET;
    AeMode aeMode = CAM_AE_MODE_NOT_SET;
    mFlashNeeded = false;

    if (mState == STATE_PREVIEW_VIDEO) {
        if (recordingBSEnable() != NO_ERROR) {
            LOGE("Error voting for buffer sharing");
        }
        mState = STATE_RECORDING;
    } else if (mState == STATE_PREVIEW_STILL) {
        /* We are in PREVIEW_STILL mode; in order to start recording
         * we first need to stop AtomISP and restart it with MODE_VIDEO
         */
        LOG2("We are in STATE_PREVIEW. Switching to STATE_VIDEO before starting to record.");
        if ((status = mISP->stop()) == NO_ERROR) {
            if ((status = mISP->start(MODE_VIDEO)) == NO_ERROR) {
                mState = STATE_RECORDING;
            } else {
                LOGE("Error starting ISP in VIDEO mode!");
            }
        } else {
            LOGE("Error stopping ISP!");
        }
    } else {
        LOGE("Error starting recording. Invalid state!");
        status = INVALID_OPERATION;
    }

    const char *pFlashMode = mParameters.get(CameraParameters::KEY_FLASH_MODE);
    if (pFlashMode != NULL && strncmp(pFlashMode, CameraParameters::FLASH_MODE_TORCH, strlen(CameraParameters::FLASH_MODE_TORCH)) == 0) {
        LOG1("Using Flash for recording!");
        mFlashNeeded = true;
        status = mISP->setTorch(TORCH_INTENSITY);
    }

    // return status and unblock message sender
    mMessageQueue.reply(MESSAGE_ID_START_RECORDING, status);
    return status;
}

status_t ControlThread::handleMessageStopRecording()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mState == STATE_RECORDING) {
        /*
         * Even if startRecording was called from PREVIEW_STILL mode, we can
         * switch back to PREVIEW_VIDEO now since we got a startRecording
         */
        status = mVideoThread->flushMessages();
        if (status != NO_ERROR)
            LOGE("Error flushing video thread");
        if (recordingBSDisable() != NO_ERROR) {
            LOGE("Error voting for disable buffer sharing");
        }
        mState = STATE_PREVIEW_VIDEO;
        if (mFlashNeeded) {
            mISP->setTorch(0);
            mFlashNeeded = false;
        }
    } else {
        LOGE("Error stopping recording. Invalid state!");
        status = INVALID_OPERATION;
    }

    // return status and unblock message sender
    mMessageQueue.reply(MESSAGE_ID_STOP_RECORDING, status);
    return status;
}

bool ControlThread::runPreFlashSequence()
{
    size_t framesTillFlashComplete = 0;
    AtomBuffer buff;
    bool ret = false;
    status_t status = NO_ERROR;
    atomisp_frame_status frameStatus = ATOMISP_FRAME_STATUS_OK;

    // Stage 1
    status = mISP->getPreviewFrame(&buff);
    if (status == NO_ERROR) {
        mISP->putPreviewFrame(&buff);
        mAAA->applyPreFlashProcess(CAM_FLASH_STAGE_NONE);
    } else {
        return ret;
    }

    // Stage 2
    status = mISP->getPreviewFrame(&buff);
    if (status == NO_ERROR) {
        mISP->putPreviewFrame(&buff);
        mAAA->applyPreFlashProcess(CAM_FLASH_STAGE_PRE);
    } else {
        return ret;
    }

    // Stage 3: get the flash-exposed preview frame
    // and let the 3A library calculate the exposure
    // settings for the flash-exposed still capture.
    // We check the frame status to make sure we use
    // the flash-exposed frame.
    status = mISP->setFlash(1);

    if (status != NO_ERROR) {
        LOGE("Failed to request pre-flash frame");
        return false;
    }

    while (++framesTillFlashComplete < FLASH_FRAME_TIMEOUT) {
        status = mISP->getPreviewFrame(&buff, &frameStatus);
        if (status == NO_ERROR) {
            mISP->putPreviewFrame(&buff);
        } else {
            return ret;
        }
        if (frameStatus == ATOMISP_FRAME_STATUS_FLASH_EXPOSED) {
            LOG1("PreFlash@Frame %d: SUCCESS    (stopping...)", framesTillFlashComplete);
            ret = true;
            break;
        }
        if (frameStatus == ATOMISP_FRAME_STATUS_FLASH_FAILED) {
            LOG1("PreFlash@Frame %d: FAILED     (stopping...)", framesTillFlashComplete);
            break;
        }
    }

    if (ret) {
        mAAA->applyPreFlashProcess(CAM_FLASH_STAGE_MAIN);
    } else {
        mAAA->apply3AProcess(true);
    }

    return ret;
}

status_t ControlThread::handleMessageTakePicture(bool clientRequest)
{
    LOG1("@%s: clientRequest = %d", __FUNCTION__, clientRequest);
    status_t status = NO_ERROR;
    AtomBuffer snapshotBuffer, postviewBuffer;
    State origState = mState;
    int width;
    int height;
    int format;
    FlashMode flashMode = CAM_AE_FLASH_MODE_NOT_SET;
    atomisp_makernote_info makerNote;

#ifndef ANDROID_2036
    if (origState == STATE_RECORDING) {
        LOGE("Video snapshot not supported!");
        return INVALID_OPERATION;
    }
#endif

    if (clientRequest) {
        if (origState == STATE_CAPTURE) {
            // Subsequent calls to takePicture, we will use previous frames, send shutter sound now
            mCallbacksThread->shutterSound();
        }
        // Notify CallbacksThread that a picture was requested, so grab one from queue
        mCallbacksThread->requestTakePicture();
        /*
         *  If the CallbacksThread has already JPEG buffers in queue, make sure we use them, before
         *  continuing to dequeue frames from ISP and encode them
         */
        if (origState == STATE_CAPTURE) {
            if (mCallbacksThread->getQueuedBuffersNum() > MAX_JPEG_BUFFERS) {
                return NO_ERROR;
            }
            // Check if ISP has free buffers we can use
            if (!mISP->dataAvailable()) {
                // If ISP has no data, do nothing and return
                return NO_ERROR;
            }
        }
    }

    if (origState != STATE_PREVIEW_STILL && origState != STATE_RECORDING && origState != STATE_CAPTURE) {
        LOGE("we only support snapshot in still preview, recording and capture modes");
        return INVALID_OPERATION;
    }
    if (origState != STATE_CAPTURE) {
        stopFaceDetection();
    }

    if (origState != STATE_RECORDING) {
        // Disable flash by default for other states than STATE_RECORDING
        mFlashNeeded = false;
    }

    if (origState == STATE_PREVIEW_STILL) {
        // This is the first call to takePicture
        // Do flash processing and stop ISP from preview mode
        const char *pFlashMode = mParameters.get(CameraParameters::KEY_FLASH_MODE);
        if (pFlashMode != NULL && strncmp(pFlashMode, CameraParameters::FLASH_MODE_ON, strlen(CameraParameters::FLASH_MODE_ON)) == 0) {
            mFlashNeeded = true;
        }

        // If flash mode is not ON, check for other modes: AUTO, DAY_SYNC, SLOW_SYNC
        if (!mFlashNeeded && mAAA->is3ASupported()) {
            flashMode = mAAA->getAeFlashMode();
            if (DetermineFlash(flashMode)) {
                mFlashNeeded = mAAA->getAeFlashNecessary();
                LOG1("In flash-mode: %d, determined flashNeeded: %d", flashMode, mFlashNeeded);
            } else {
                mFlashNeeded = false;
            }
            if (mFlashNeeded) {
                if (mAAA->getAeMode() != CAM_AE_MODE_MANUAL) {
                    mFlashNeeded = runPreFlashSequence();
                }
            }
        }
        status = stopPreviewCore();
        if (status != NO_ERROR) {
            LOGE("Error stopping preview!");
            return status;
        }
        mState = STATE_CAPTURE;
    }

    // Get the current params
    mParameters.getPictureSize(&width, &height);
    format = mISP->getSnapshotPixelFormat();
    if (origState == STATE_RECORDING) {
        // override picture size to video size if recording
        int vidWidth, vidHeight;
        mISP->getVideoSize(&vidWidth, &vidHeight);
        if (width != vidWidth || height != vidHeight) {
            LOGW("Warning overriding snapshot size=%d,%d to %d,%d",
                    width, height, vidWidth, vidHeight);
            width = vidWidth;
            height = vidHeight;
        }
    }

    status = mISP->getMakerNote(&makerNote);
    if (status != NO_ERROR) {
        LOGW("Could not get maker note information!");
    }

    // Configure PictureThread
    mPictureThread->setPictureFormat(format);
    if (origState == STATE_PREVIEW_STILL) {
        mPictureThread->initialize(mParameters, makerNote, mFlashNeeded);

    } else if (origState == STATE_RECORDING) { // STATE_RECORDING

        // Picture thread uses snapshot-size to configure itself. However,
        // if in recording mode we need to override snapshot with video-size.
        CameraParameters copyParams = mParameters;
        copyParams.setPictureSize(width, height); // make sure picture size is same as video size
        // mFlashNeeded is set in handleMessageStartRecording for STATE_RECORDING
        mPictureThread->initialize(copyParams, makerNote, mFlashNeeded);
    }

    if (origState == STATE_PREVIEW_STILL) {
        // Configure and start the ISP
        mISP->setSnapshotFrameFormat(width, height, format);
        mISP->setSnapshotNum(NUM_BURST_BUFFERS);
        if (format == V4L2_PIX_FMT_NV12) {
            // Try to use buffer sharing
            void* snapshotBufferPtr;
            status = mPictureThread->getSharedBuffers(width, height, &snapshotBufferPtr, NUM_BURST_BUFFERS);
            if (status == NO_ERROR) {
                status = mISP->setSnapshotBuffers(snapshotBufferPtr, NUM_BURST_BUFFERS);
                if (status == NO_ERROR) {
                    LOG1("Using shared buffers for snapshot");
                } else {
                    LOGW("Cannot set shared buffers in atomisp, using internal buffers!");
                }
            } else {
                LOGW("Cannot get shared buffers from libjpeg, using internal buffers!");
            }
        } else {
            LOG1("Using internal buffers for snapshot");
        }
        if ((status = mISP->start(MODE_CAPTURE)) != NO_ERROR) {
            LOGE("Error starting the ISP driver in CAPTURE mode!");
            return status;
        }

        /*
         *  If the current camera does not have 3A, then we should skip the first
         *  frames in order to allow the sensor to warm up.
         */
        if (!mAAA->is3ASupported()) {
            for (size_t i = 0; i < NUM_WARMUP_FRAMES; i++) {
                if ((status = mISP->getSnapshot(&snapshotBuffer, &postviewBuffer)) != NO_ERROR) {
                    LOGE("Error in grabbing warm-up frame %d!", i);
                    return status;
                }
                status = mISP->putSnapshot(&snapshotBuffer, &postviewBuffer);
                if (status == DEAD_OBJECT) {
                    LOG1("Stale snapshot buffer returned to ISP");
                } else if (status != NO_ERROR) {
                    LOGE("Error in putting warm-up frame %d!", i);
                    return status;
                }
            }
        }
    }

    if (mState == STATE_CAPTURE) {
        // Turn on flash
        if (mFlashNeeded) {
            LOG1("Requesting flash");
            if (mISP->setFlash(1) != NO_ERROR) {
                LOGE("Failed to enable the Flash!");
            }
        } else if (DetermineFlash(flashMode)) {
            mISP->setFlashIndicator(TORCH_INTENSITY);
        }

        // Get the snapshot
        if ((status = mISP->getSnapshot(&snapshotBuffer, &postviewBuffer)) != NO_ERROR) {
            LOGE("Error in grabbing snapshot!");
            return status;
        }

        if (origState != STATE_CAPTURE) {
            // First time call: send request to play the Shutter Sound
            mCallbacksThread->shutterSound();
        }

        // TODO: here we should display the picture using PreviewThread

        // Turn off flash
        if (!mFlashNeeded && DetermineFlash(flashMode)) {
            mISP->setFlashIndicator(0);
        }
    }

    /*
     * Handle Red-Eye removal. The Red-Eye removal should be done in a separate thread, so if we are
     * in burst-capture mode, we can do: grab frames in ControlThread, Red-Eye removal in AAAThread
     * and JPEG encoding in Picture thread, all in parallel.
     */
    if (mAAA->is3ASupported() && mFlashNeeded && mAAA->getRedEyeRemoval()) {
        // tell 3A thread to remove red-eye
        if (mState == STATE_CAPTURE) {
            status = m3AThread->applyRedEyeRemoval(&snapshotBuffer, &postviewBuffer, width, height, format);
        } else {
            mCoupledBuffers[mLastRecordingBuffIndex].videoSnapshotBuff = true;
            status = m3AThread->applyRedEyeRemoval(&snapshotBuffer, NULL, width, height, format);
        }

        if (status == NO_ERROR) {
            return status;
        } else {
            LOGE("Red-Eye removal failed! Continue to encode picture...");
        }
    }

    // Do jpeg encoding
    if (mState == STATE_CAPTURE) {
        status = mPictureThread->encode(&snapshotBuffer, &postviewBuffer);
    } else {
        // If we are in video mode we simply use the recording buffer for picture encoding
        // No need to stop, reconfigure, and restart the ISP
        mCoupledBuffers[mLastRecordingBuffIndex].videoSnapshotBuff = true;
        status = mPictureThread->encode(&mCoupledBuffers[mLastRecordingBuffIndex].recordingBuff);
    }

    return status;
}

status_t ControlThread::handleMessageCancelPicture()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // TODO: implement

    return status;
}

status_t ControlThread::handleMessageAutoFocus()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    mFlashNeeded = false;
    // Implement pre auto-focus functions
    if (mAAA->is3ASupported()) {
        const char *pFlashMode = mParameters.get(CameraParameters::KEY_FLASH_MODE);
        if (pFlashMode != NULL && strncmp(pFlashMode, CameraParameters::FLASH_MODE_ON, strlen(CameraParameters::FLASH_MODE_ON)) == 0) {
            mFlashNeeded = true;
        }

        FlashMode flashMode = mAAA->getAeFlashMode();
        if (!mFlashNeeded && DetermineFlash(flashMode)) {
            // Check the other modes
            LOG1("Flash mode = %d", flashMode);
            if (mAAA->getAeFlashNecessary()) {
                mFlashNeeded = true;
            }
        }

        if (mFlashNeeded) {
            LOG1("Using Torch for auto-focus");
            mISP->setTorch(TORCH_INTENSITY);
        }
    }

    //If the apps call autoFocus(AutoFocusCallback), the camera will stop sending face callbacks.
    // The last face callback indicates the areas used to do autofocus. After focus completes,
    // face detection will resume sending face callbacks.
    //If the apps call cancelAutoFocus(), the face callbacks will also resume.
    LOG2("auto focus is on");
    if (mFaceDetectionActive)
        disableMsgType(CAMERA_MSG_PREVIEW_METADATA);
    // Auto-focus should be done in AAAThread, so send a message directly to it
    status = m3AThread->autoFocus();

    // If start auto-focus failed and we enabled torch, disable it now
    if (status != NO_ERROR && mFlashNeeded) {
        mISP->setTorch(0);
        mFlashNeeded = false;
    }

    return status;
}

status_t ControlThread::handleMessageCancelAutoFocus()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    status = m3AThread->cancelAutoFocus();
    LOG2("auto focus is off");
    if (mFaceDetectionActive)
        enableMsgType(CAMERA_MSG_PREVIEW_METADATA);
    if (mFlashNeeded) {
        mISP->setTorch(0);
        mFlashNeeded = false;
    }
    /*
     * The normal autoFocus sequence is:
     * - camera client is calling autoFocus (we run the AF sequence and lock AF)
     * - camera client is calling:
     *     - takePicture: AF is locked, so the picture will have the focus established
     *       in previous step. In this case, we have to reset the auto-focus to enabled
     *       when the camera client will call startPreview.
     *     - cancelAutoFocus: AF is locked, camera client no longer wants this focus position
     *       so we should switch back to auto-focus in 3A library
     */
    if (mAAA->is3ASupported()) {
        mAAA->setAfEnabled(true);
    }
    return status;
}

status_t ControlThread::handleMessageReleaseRecordingFrame(MessageReleaseRecordingFrame *msg)
{
    LOG2("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    if (mState == STATE_RECORDING) {
        AtomBuffer *recBuff = findRecordingBuffer(msg->buff);
        if (recBuff == NULL) {
            // This may happen with buffer sharing. When the omx component is stopped
            // it disables buffer sharing and deallocates its buffers. Internally we check
            // to see if sharing was disabled then we restart the ISP with new buffers. In
            // the mean time, the app is returning us shared buffers when we are no longer
            // using them.
            LOGE("Could not find recording buffer: %p", msg->buff);
            return DEAD_OBJECT;
        }
        int curBuff = recBuff->id;
        LOG2("Recording buffer released from encoder, buff id = %d", curBuff);
        if (mCoupledBuffers && curBuff < mNumBuffers) {
            mCoupledBuffers[curBuff].recordingBuffReturned = true;
            status = queueCoupledBuffers(curBuff);
        }
    }
    return status;
}

status_t ControlThread::handleMessagePreviewDone(MessagePreviewDone *msg)
{

    LOG2("handle preview frame done buff id = %d", msg->buff.id);
    if (!mISP->isBufferValid(&msg->buff))
        return DEAD_OBJECT;
    status_t status = NO_ERROR;
    if (m_pFaceDetector !=0 && mFaceDetectionActive) {
        LOG2("m_pFace = 0x%p, active=%s", m_pFaceDetector, mFaceDetectionActive?"true":"false");
        int width, height;
        mParameters.getPreviewSize(&width, &height);
        LOG2("sending frame data = %p", msg->buff.buff->data);
        msg->buff.owner = this;
        msg->buff.type = MODE_PREVIEW;
        if (m_pFaceDetector->sendFrame(&msg->buff, width, height) < 0) {
            msg->buff.owner = 0;
            releasePreviewFrame(&msg->buff);
        }
    }else
    {
       releasePreviewFrame(&msg->buff);
    }
    return NO_ERROR;
}

status_t ControlThread::handleMessageReleasePreviewFrame(MessageReleasePreviewFrame *msg)
{
    LOG2("handle preview frame release buff id = %d", msg->buff.id);
    status_t status = NO_ERROR;
    if (mState == STATE_PREVIEW_STILL) {
        status = mISP->putPreviewFrame(&msg->buff);
        if (status == DEAD_OBJECT) {
            LOG2("Stale preview buffer returned to ISP");
        } else if (status != NO_ERROR) {
            LOGE("Error putting preview frame to ISP");
        }
    } else if (mState == STATE_PREVIEW_VIDEO || mState == STATE_RECORDING) {
        int curBuff = msg->buff.id;
        if (mCoupledBuffers && curBuff < mNumBuffers) {
            mCoupledBuffers[curBuff].previewBuffReturned = true;
            status = queueCoupledBuffers(curBuff);
        }
    }
    return status;
}

status_t ControlThread::queueCoupledBuffers(int coupledId)
{
    LOG2("@%s: coupledId = %d", __FUNCTION__, coupledId);
    status_t status = NO_ERROR;

    CoupledBuffer *buff = &mCoupledBuffers[coupledId];

    if (!buff->previewBuffReturned || !buff->recordingBuffReturned ||
            (buff->videoSnapshotBuff && !buff->videoSnapshotBuffReturned))
        return NO_ERROR;
    LOG2("Putting buffer back to ISP, coupledId = %d",  coupledId);
    status = mISP->putRecordingFrame(&buff->recordingBuff);
    if (status == NO_ERROR) {
        status = mISP->putPreviewFrame(&buff->previewBuff);
        if (status == DEAD_OBJECT) {
            LOG1("Stale preview buffer returned to ISP");
        } else if (status != NO_ERROR) {
            LOGE("Error putting preview frame to ISP");
        }
    } else if (status == DEAD_OBJECT) {
        LOG1("Stale recording buffer returned to ISP");
    } else {
        LOGE("Error putting recording frame to ISP");
    }

    return status;
}

status_t ControlThread::handleMessagePictureDone(MessagePicture *msg)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mState == STATE_RECORDING) {
        int curBuff = msg->snapshotBuf.id;
        if (mCoupledBuffers && curBuff < mNumBuffers) {
            mCoupledBuffers[curBuff].videoSnapshotBuffReturned = true;
            status = queueCoupledBuffers(curBuff);
            mCoupledBuffers[curBuff].videoSnapshotBuffReturned = false;
            mCoupledBuffers[curBuff].videoSnapshotBuff = false;
        }
    } else if (mState == STATE_CAPTURE) {
        // Return the picture frames back to ISP
        status = mISP->putSnapshot(&msg->snapshotBuf, &msg->postviewBuf);
        if (status == DEAD_OBJECT) {
            LOG1("Stale snapshot buffer returned to ISP");
        } else if (status != NO_ERROR) {
            LOGE("Error in putting snapshot!");
            return status;
        }
    }

    return status;
}

status_t ControlThread::handleMessageRedEyeRemovalDone(MessagePicture *msg)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    status = mPictureThread->encode(&msg->snapshotBuf, &msg->postviewBuf);

    return status;
}

status_t ControlThread::handleMessageAutoFocusDone()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    if (mFaceDetectionActive)
        enableMsgType(CAMERA_MSG_PREVIEW_METADATA);
    // Implement post auto-focus functions
    if (mFlashNeeded) {
        mISP->setTorch(0);
        mFlashNeeded = false;
    }

    return status;
}

status_t ControlThread::validateParameters(const CameraParameters *params)
{
    LOG1("@%s: params = %p", __FUNCTION__, params);
    // PREVIEW
    int previewWidth, previewHeight;
    params->getPreviewSize(&previewWidth, &previewHeight);
    if (previewWidth <= 0 || previewHeight <= 0) {
        LOGE("bad preview size");
        return BAD_VALUE;
    }

    int minFPS, maxFPS;
    params->getPreviewFpsRange(&minFPS, &maxFPS);
    if (minFPS == maxFPS || minFPS > maxFPS) {
        LOGE("invalid fps range [%d,%d]", minFPS, maxFPS);
        return BAD_VALUE;
    }

    // VIDEO
    int videoWidth, videoHeight;
    params->getPreviewSize(&videoWidth, &videoHeight);
    if (videoWidth <= 0 || videoHeight <= 0) {
        LOGE("bad video size");
        return BAD_VALUE;
    }

    // SNAPSHOT
    int pictureWidth, pictureHeight;
    params->getPreviewSize(&pictureWidth, &pictureHeight);
    if (pictureWidth <= 0 || pictureHeight <= 0) {
        LOGE("bad picture size");
        return BAD_VALUE;
    }

    // MISCELLANEOUS
    // TODO: implement validation for other features not listed above

    return NO_ERROR;
}

status_t ControlThread::processDynamicParameters(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int oldZoom = oldParams->getInt(CameraParameters::KEY_ZOOM);
    int newZoom = newParams->getInt(CameraParameters::KEY_ZOOM);
    bool videoMode = isParameterSet(CameraParameters::KEY_RECORDING_HINT) ? true : false;

    if (oldZoom != newZoom)
        status = mISP->setZoom(newZoom);


    if (!videoMode) {
        // Pre-allocate picture buffers only when not recording
        int picWidth, picHeight;
        mParameters.getPictureSize(&picWidth, &picHeight);
        status = mPictureThread->allocSharedBuffers(picWidth, picHeight, NUM_BURST_BUFFERS);
        if (status != NO_ERROR) {
            LOGW("Could not pre-allocate picture buffers!");
        }
    }

    // We won't take care of the status returned by the following calls since
    // failure of setting one parameter should not stop us setting the other parameters

    // Colour effect
    status = processParamEffect(oldParams, newParams);

    if (mAAA->is3ASupported()) {
        if (status == NO_ERROR) {
            // Scene Mode
            status = processParamFlash(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            // Scene Mode
            status = processParamSceneMode(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            //Focus Mode
            status = processParamFocusMode(oldParams, newParams);
        }

        if (status == NO_ERROR || !mFaceDetectionActive) {
            // white balance
            status = processParamWhiteBalance(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            // red-eye removal
            status = processParamRedEyeMode(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            // ae lock
            status = processParamAELock(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            // af lock
            status = processParamAFLock(oldParams, newParams);
        }

        if (status == NO_ERROR) {
            // awb lock
            status = processParamAWBLock(oldParams, newParams);
        }

        if (!mFaceDetectionActive && status == NO_ERROR) {
            // customize metering
            status = processParamSetMeteringAreas(oldParams, newParams);
        }
    }
    return status;
}

status_t ControlThread::processParamAFLock(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // af lock mode
    const char* oldValue = oldParams->get(CameraParameters::KEY_AUTO_FOCUS_LOCK);
    const char* newValue = newParams->get(CameraParameters::KEY_AUTO_FOCUS_LOCK);
    if (newValue && oldValue && strncmp(newValue, oldValue, MAX_PARAM_VALUE_LENGTH) != 0) {
        bool af_lock;

        if(!strncmp(newValue, CameraParameters::TRUE, strlen(CameraParameters::TRUE))) {
            af_lock = true;
        } else if(!strncmp(newValue, CameraParameters::FALSE, strlen(CameraParameters::FALSE))) {
            af_lock = false;
        } else {
            LOGE("Invalid value received for %s: %s", CameraParameters::KEY_AUTO_FOCUS_LOCK, newValue);
            return INVALID_OPERATION;
        }
        status = mAAA->setAfLock(af_lock);

        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_AUTO_FOCUS_LOCK, newValue);
        }
    }

    return status;
}

status_t ControlThread::processParamAWBLock(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // awb lock mode
    const char* oldValue = oldParams->get(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK);
    const char* newValue = newParams->get(CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK);
    if (newValue && oldValue && strncmp(newValue, oldValue, MAX_PARAM_VALUE_LENGTH) != 0) {
        bool awb_lock;

        if(!strncmp(newValue, CameraParameters::TRUE, strlen(CameraParameters::TRUE))) {
            awb_lock = true;
        } else if(!strncmp(newValue, CameraParameters::FALSE, strlen(CameraParameters::FALSE))) {
            awb_lock = false;
        } else {
            LOGE("Invalid value received for %s: %s", CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK, newValue);
            return INVALID_OPERATION;
        }
        status = mAAA->setAwbLock(awb_lock);

        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_AUTO_WHITEBALANCE_LOCK, newValue);
        }
    }

    return status;
}

status_t ControlThread::processParamAELock(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // ae lock mode
    const char* oldValue = oldParams->get(CameraParameters::KEY_AUTO_EXPOSURE_LOCK);
    const char* newValue = newParams->get(CameraParameters::KEY_AUTO_EXPOSURE_LOCK);

    if (newValue && oldValue && strncmp(newValue, oldValue, MAX_PARAM_VALUE_LENGTH) != 0) {
        bool ae_lock;

        if(!strncmp(newValue, CameraParameters::TRUE, strlen(CameraParameters::TRUE))) {
            ae_lock = true;
        } else  if(!strncmp(newValue, CameraParameters::FALSE, strlen(CameraParameters::FALSE))) {
            ae_lock = false;
        } else {
            LOGE("Invalid value received for %s: %s", CameraParameters::KEY_AUTO_EXPOSURE_LOCK, newValue);
            return INVALID_OPERATION;
        }

        status =  mAAA->setAeLock(ae_lock);
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_AUTO_EXPOSURE_LOCK, newValue);
        }
    }

    return status;
}

status_t ControlThread::processParamFlash(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldValue = oldParams->get(CameraParameters::KEY_FLASH_MODE);
    const char* newValue = newParams->get(CameraParameters::KEY_FLASH_MODE);
    if (newValue && oldValue && strncmp(newValue, oldValue, MAX_PARAM_VALUE_LENGTH) != 0) {
        FlashMode flash = CAM_AE_FLASH_MODE_AUTO;
        if(!strncmp(newValue, CameraParameters::FLASH_MODE_AUTO, strlen(CameraParameters::FLASH_MODE_AUTO)))
            flash = CAM_AE_FLASH_MODE_AUTO;
        else if(!strncmp(newValue, CameraParameters::FLASH_MODE_OFF, strlen(CameraParameters::FLASH_MODE_OFF)))
            flash = CAM_AE_FLASH_MODE_OFF;
        else if(!strncmp(newValue, CameraParameters::FLASH_MODE_ON, strlen(CameraParameters::FLASH_MODE_ON)))
            flash = CAM_AE_FLASH_MODE_ON;
        else if(!strncmp(newValue, CameraParameters::FLASH_MODE_TORCH, strlen(CameraParameters::FLASH_MODE_TORCH)))
            flash = CAM_AE_FLASH_MODE_TORCH;
        else if(!strncmp(newValue, CameraParameters::FLASH_MODE_SLOW_SYNC, strlen(CameraParameters::FLASH_MODE_SLOW_SYNC)))
            flash = CAM_AE_FLASH_MODE_SLOW_SYNC;
        else if(!strncmp(newValue, CameraParameters::FLASH_MODE_DAY_SYNC, strlen(CameraParameters::FLASH_MODE_DAY_SYNC)))
            flash = CAM_AE_FLASH_MODE_DAY_SYNC;

        status = mAAA->setAeFlashMode(flash);
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_FLASH_MODE, newValue);
        }
    }
    return status;
}

status_t ControlThread::processParamEffect(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldEffect = oldParams->get(CameraParameters::KEY_EFFECT);
    const char* newEffect = newParams->get(CameraParameters::KEY_EFFECT);
    if (newEffect && oldEffect && strncmp(newEffect, oldEffect, MAX_PARAM_VALUE_LENGTH) != 0) {
        v4l2_colorfx effect = V4L2_COLORFX_NONE;
        if(!strncmp(newEffect, CameraParameters::EFFECT_MONO, strlen(CameraParameters::EFFECT_MONO)))
            effect = V4L2_COLORFX_BW;
        else if(!strncmp(newEffect, CameraParameters::EFFECT_NEGATIVE, strlen(CameraParameters::EFFECT_NEGATIVE)))
            effect = V4L2_COLORFX_NEGATIVE;
        else if(!strncmp(newEffect, CameraParameters::EFFECT_SEPIA, strlen(CameraParameters::EFFECT_SEPIA)))
            effect = V4L2_COLORFX_SEPIA;

        status = mISP->setColorEffect(effect);
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_EFFECT, newEffect);
        }
    }
    return status;
}

status_t ControlThread::processParamSceneMode(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldScene = oldParams->get(CameraParameters::KEY_SCENE_MODE);
    const char* newScene = newParams->get(CameraParameters::KEY_SCENE_MODE);
    if (newScene && oldScene && strncmp(newScene, oldScene, MAX_PARAM_VALUE_LENGTH) != 0) {
        SceneMode sceneMode = CAM_AE_SCENE_MODE_AUTO;
        if (!strncmp (newScene, CameraParameters::SCENE_MODE_PORTRAIT, strlen(CameraParameters::SCENE_MODE_PORTRAIT))) {
            sceneMode = CAM_AE_SCENE_MODE_PORTRAIT;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_AUTO);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_AUTO);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_SPORTS, strlen(CameraParameters::SCENE_MODE_SPORTS))) {
            sceneMode = CAM_AE_SCENE_MODE_SPORTS;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_INFINITY);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_LANDSCAPE, strlen(CameraParameters::SCENE_MODE_LANDSCAPE))) {
            sceneMode = CAM_AE_SCENE_MODE_LANDSCAPE;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_INFINITY);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_OUTDOOR);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_NIGHT, strlen(CameraParameters::SCENE_MODE_NIGHT))) {
            sceneMode = CAM_AE_SCENE_MODE_NIGHT;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_NIGHT_PORTRAIT, strlen(CameraParameters::SCENE_MODE_NIGHT_PORTRAIT))) {
            sceneMode = CAM_AE_SCENE_MODE_NIGHT_PORTRAIT;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_ON);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_FIREWORKS, strlen(CameraParameters::SCENE_MODE_FIREWORKS))) {
            sceneMode = CAM_AE_SCENE_MODE_FIREWORKS;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_INFINITY);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_OFF);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_OFF);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else if (!strncmp (newScene, CameraParameters::SCENE_MODE_TEXT, strlen(CameraParameters::SCENE_MODE_TEXT))) {
            sceneMode = CAM_AE_SCENE_MODE_TEXT;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_MACRO);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_AUTO);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_AUTO);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        } else {
            if (strncmp (newScene, CameraParameters::SCENE_MODE_AUTO, strlen(CameraParameters::SCENE_MODE_AUTO))) {
                LOG1("Unsupported %s: %s. Using AUTO!", CameraParameters::KEY_SCENE_MODE, newScene);
            }
            sceneMode = CAM_AE_SCENE_MODE_AUTO;
            newParams->set(CameraParameters::KEY_FOCUS_MODE, CameraParameters::FOCUS_MODE_AUTO);
            newParams->set(CameraParameters::KEY_WHITE_BALANCE, CameraParameters::WHITE_BALANCE_AUTO);
            newParams->set(CameraParameters::KEY_ANTIBANDING, CameraParameters::ANTIBANDING_AUTO);
            newParams->set(CameraParameters::KEY_FLASH_MODE, CameraParameters::FLASH_MODE_AUTO);
            newParams->set(CameraParameters::KEY_AWB_MAPPING_MODE, CameraParameters::AWB_MAPPING_AUTO);
            newParams->set(CameraParameters::KEY_AE_METERING_MODE, CameraParameters::AE_METERING_MODE_AUTO);
            newParams->set(CameraParameters::KEY_BACK_LIGHTING_CORRECTION_MODE, CameraParameters::BACK_LIGHT_COORECTION_OFF);
        }

        mAAA->setAeSceneMode(sceneMode);
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_SCENE_MODE, newScene);
        }
    }
    return status;
}

bool ControlThread::verifyCameraWindow(const CameraWindow &win)
{
    if (win.x_right <= win.x_left ||
        win.y_bottom <= win.y_top)
        return false;
    return true;
}

void ControlThread::preSetCameraWindows(CameraWindow* focusWindows, size_t winCount)
{
    LOG1("@%s", __FUNCTION__);
    // Camera KEY_FOCUS_AREAS Coordinates range from -1000 to 1000.
    if (winCount > 0) {
        int width;
        int height;
        const int FOCUS_AREAS_X_OFFSET = 1000;
        const int FOCUS_AREAS_Y_OFFSET = 1000;
        const int FOCUS_AREAS_WIDTH = 2000;
        const int FOCUS_AREAS_HEIGHT = 2000;
        const int WINDOWS_TOTAL_WEIGHT = 16;
        mParameters.getPreviewSize(&width, &height);
        size_t windowsWeight = 0;
        for(size_t i = 0; i < winCount; i++){
            windowsWeight += focusWindows[i].weight;
        }
        if(!windowsWeight)
            windowsWeight = 1;

        size_t weight_sum = 0;
        for(size_t i =0; i < winCount; i++) {
            // skip all zero value
            focusWindows[i].x_left = (focusWindows[i].x_left + FOCUS_AREAS_X_OFFSET) * (width - 1) / FOCUS_AREAS_WIDTH;
            focusWindows[i].x_right = (focusWindows[i].x_right + FOCUS_AREAS_X_OFFSET) * (width - 1) / FOCUS_AREAS_WIDTH;
            focusWindows[i].y_top = (focusWindows[i].y_top + FOCUS_AREAS_Y_OFFSET) * (height - 1) / FOCUS_AREAS_HEIGHT;
            focusWindows[i].y_bottom = (focusWindows[i].y_bottom + FOCUS_AREAS_Y_OFFSET) * (height - 1) / FOCUS_AREAS_HEIGHT;
            focusWindows[i].weight = focusWindows[i].weight * WINDOWS_TOTAL_WEIGHT / windowsWeight;
            weight_sum += focusWindows[i].weight;
            LOG1("Preset camera window %d: (%d,%d,%d,%d,%d)",
                    i,
                    focusWindows[i].x_left,
                    focusWindows[i].y_top,
                    focusWindows[i].x_right,
                    focusWindows[i].y_bottom,
                    focusWindows[i].weight);
        }
        //weight sum value should be WINDOWS_TOTAL_WEIGHT
        focusWindows[winCount-1].weight += WINDOWS_TOTAL_WEIGHT - weight_sum;
    }
}

status_t ControlThread::processParamFocusMode(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldFocus = oldParams->get(CameraParameters::KEY_FOCUS_MODE);
    const char* newFocus = newParams->get(CameraParameters::KEY_FOCUS_MODE);
    if (newFocus && oldFocus && strncmp(newFocus, oldFocus, MAX_PARAM_VALUE_LENGTH) != 0) {
        AfMode afMode = CAM_AF_MODE_AUTO;

        if(!strncmp(newFocus, CameraParameters::FOCUS_MODE_AUTO, strlen(CameraParameters::FOCUS_MODE_AUTO))) {
            afMode = CAM_AF_MODE_AUTO;
        } else if(!strncmp(newFocus, CameraParameters::FOCUS_MODE_INFINITY, strlen(CameraParameters::FOCUS_MODE_INFINITY))) {
            afMode = CAM_AF_MODE_INFINITY;
        } else if(!strncmp(newFocus, CameraParameters::FOCUS_MODE_MACRO, strlen(CameraParameters::FOCUS_MODE_MACRO))) {
            afMode = CAM_AF_MODE_MACRO;
        } else if(!strncmp(newFocus, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO, strlen(CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO))) {
            afMode = CAM_AF_MODE_AUTO;
        } else {
            afMode = CAM_AF_MODE_MANUAL;
        }

        status = mAAA->setAfEnabled(true);
        if (status == NO_ERROR) {
            status = mAAA->setAfMode(afMode);
        }
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_FOCUS_MODE, newFocus);
        }
    }

    // Handling the window information in auto, macro and continuous video mode.
    // If focus window is set, we will actually use the touch mode!
    if ((!strncmp(newFocus, CameraParameters::FOCUS_MODE_AUTO, strlen(CameraParameters::FOCUS_MODE_AUTO))) ||
            (!strncmp(newFocus, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO, strlen(CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO))) ||
            (!strncmp(newFocus, CameraParameters::FOCUS_MODE_MACRO, strlen(CameraParameters::FOCUS_MODE_MACRO)))) {

        // By default we will use auto or macro mode
        AfMode newAfMode = CAM_AF_MODE_AUTO;
        if (!strncmp(newFocus, CameraParameters::FOCUS_MODE_MACRO, strlen(CameraParameters::FOCUS_MODE_MACRO)))
            newAfMode = CAM_AF_MODE_MACRO;

        // See if any focus windows are set
        const char *pFocusWindows = newParams->get(CameraParameters::KEY_FOCUS_AREAS);
        const size_t maxWindows = mAAA->getAfMaxNumWindows();
        size_t winCount = 0;
        CameraWindow focusWindows[maxWindows];

        if (!mFaceDetectionActive && pFocusWindows && (maxWindows > 0) && (strlen(pFocusWindows) > 0)) {
            LOG1("Scanning AF windows from params: %s", pFocusWindows);
            const char *argTail = pFocusWindows;
            while (argTail && winCount < maxWindows) {
                // String format: "(topleftx,toplefty,bottomrightx,bottomrighty,weight),(...)"
                int i = sscanf(argTail, "(%d,%d,%d,%d,%d)",
                        &focusWindows[winCount].x_left,
                        &focusWindows[winCount].y_top,
                        &focusWindows[winCount].x_right,
                        &focusWindows[winCount].y_bottom,
                        &focusWindows[winCount].weight);
                if (i != 5)
                    break;
                bool verified = verifyCameraWindow(focusWindows[winCount]);
                LOG1("\tWindow %d (%d,%d,%d,%d,%d) [%s]",
                        winCount,
                        focusWindows[winCount].x_left,
                        focusWindows[winCount].y_top,
                        focusWindows[winCount].x_right,
                        focusWindows[winCount].y_bottom,
                        focusWindows[winCount].weight,
                        (verified)?"GOOD":"IGNORED");
                argTail = strchr(argTail + 1, '(');
                if (verified) {
                    winCount++;
                } else {
                    LOGW("Ignoring invalid focus area: (%d,%d,%d,%d,%d)",
                            focusWindows[winCount].x_left,
                            focusWindows[winCount].y_top,
                            focusWindows[winCount].x_right,
                            focusWindows[winCount].y_bottom,
                            focusWindows[winCount].weight);
                }
            }
            // Looks like focus window(s) were set, so should use touch focus mode
            if (winCount > 0) {
                newAfMode = CAM_AF_MODE_TOUCH;
            }
        }

        // See if we have to change the actual mode (it could be correct already)
        AfMode curAfMode = mAAA->getAfMode();
        if (curAfMode != newAfMode)
            mAAA->setAfMode(newAfMode);

        // If in touch mode, we set the focus windows now
        if (newAfMode == CAM_AF_MODE_TOUCH) {
            preSetCameraWindows(focusWindows, winCount);
            if (mAAA->setAfWindows(focusWindows, winCount) != NO_ERROR) {
                // If focus windows couldn't be set, auto mode is used
                // (AfSetWindowMulti has its own safety checks for coordinates)
                LOGE("Could not set AF windows. Resseting the AF back to %d", curAfMode);
                mAAA->setAfMode(curAfMode);
            }
        }
    }
    return status;
}

status_t ControlThread:: processParamSetMeteringAreas(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const size_t maxWindows = mAAA->getAfMaxNumWindows();
    CameraWindow meteringWindows[maxWindows];
    const char *pMeteringWindows = newParams->get(CameraParameters::KEY_METERING_AREAS);
    if (pMeteringWindows && (maxWindows > 0) && (strlen(pMeteringWindows) > 0)) {
        LOG1("Scanning AE metering from params: %s", pMeteringWindows);
        const char *argTail = pMeteringWindows;
        size_t winCount = 0;
        while (argTail && winCount < maxWindows) {
            // String format: "(topleftx,toplefty,bottomrightx,bottomrighty,weight),(...)"
            int len = sscanf(argTail, "(%d,%d,%d,%d,%d)",
                    &meteringWindows[winCount].x_left,
                    &meteringWindows[winCount].y_top,
                    &meteringWindows[winCount].x_right,
                    &meteringWindows[winCount].y_bottom,
                    &meteringWindows[winCount].weight);
            if (len != 5)
                break;
            bool verified = verifyCameraWindow(meteringWindows[winCount]);
            LOG1("\tWindow %d (%d,%d,%d,%d,%d) [%s]",
                    winCount,
                    meteringWindows[winCount].x_left,
                    meteringWindows[winCount].y_top,
                    meteringWindows[winCount].x_right,
                    meteringWindows[winCount].y_bottom,
                    meteringWindows[winCount].weight,
                    (verified)?"GOOD":"IGNORED");
            argTail = strchr(argTail + 1, '(');
            if (verified) {
                winCount++;
            } else {
                LOGW("Ignoring invalid metering area: (%d,%d,%d,%d,%d)",
                        meteringWindows[winCount].x_left,
                        meteringWindows[winCount].y_top,
                        meteringWindows[winCount].x_right,
                        meteringWindows[winCount].y_bottom,
                        meteringWindows[winCount].weight);
            }
        }
        // Looks like metering window(s) were set
        if (winCount > 0) {
            preSetCameraWindows(meteringWindows, winCount);
            if (mAAA->setAfWindows(meteringWindows, winCount) != NO_ERROR) {
                mAAA->setAeMeteringMode(CAM_AE_METERING_MODE_SPOT);
            }
        }
    }
    return status;
}

status_t ControlThread::processParamWhiteBalance(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldWb = oldParams->get(CameraParameters::KEY_WHITE_BALANCE);
    const char* newWb = newParams->get(CameraParameters::KEY_WHITE_BALANCE);
    if (newWb && oldWb && strncmp(newWb, oldWb, MAX_PARAM_VALUE_LENGTH) != 0) {
        AwbMode wbMode = CAM_AWB_MODE_AUTO;

        if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_AUTO, strlen(CameraParameters::WHITE_BALANCE_AUTO))) {
            wbMode = CAM_AWB_MODE_AUTO;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_INCANDESCENT, strlen(CameraParameters::WHITE_BALANCE_INCANDESCENT))) {
            wbMode = CAM_AWB_MODE_WARM_INCANDESCENT;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_FLUORESCENT, strlen(CameraParameters::WHITE_BALANCE_FLUORESCENT))) {
            wbMode = CAM_AWB_MODE_FLUORESCENT;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT, strlen(CameraParameters::WHITE_BALANCE_WARM_FLUORESCENT))) {
            wbMode = CAM_AWB_MODE_WARM_FLUORESCENT;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_DAYLIGHT, strlen(CameraParameters::WHITE_BALANCE_DAYLIGHT))) {
            wbMode = CAM_AWB_MODE_DAYLIGHT;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT, strlen(CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT))) {
            wbMode = CAM_AWB_MODE_CLOUDY;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_TWILIGHT, strlen(CameraParameters::WHITE_BALANCE_TWILIGHT))) {
            wbMode = CAM_AWB_MODE_SUNSET;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_SHADE, strlen(CameraParameters::WHITE_BALANCE_SHADE))) {
            wbMode = CAM_AWB_MODE_SHADOW;
        } else if(!strncmp(newWb, CameraParameters::WHITE_BALANCE_MANUAL, strlen(CameraParameters::WHITE_BALANCE_MANUAL))) {
            wbMode = CAM_AWB_MODE_MANUAL_INPUT;
        }

        status = mAAA->setAwbMode(wbMode);

        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_WHITE_BALANCE, newWb);
        }
    }
    return status;
}

status_t ControlThread::processParamRedEyeMode(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    const char* oldRedEye = oldParams->get(CameraParameters::KEY_RED_EYE_MODE);
    const char* newRedEye = newParams->get(CameraParameters::KEY_RED_EYE_MODE);
    if (newRedEye && oldRedEye && strncmp(newRedEye, oldRedEye, MAX_PARAM_VALUE_LENGTH) != 0) {
        bool doRedEyeRemoval = true;

        if(!strncmp(newRedEye, CameraParameters::RED_EYE_REMOVAL_OFF, strlen(CameraParameters::RED_EYE_REMOVAL_OFF)))
            doRedEyeRemoval= false;

        status = mAAA->setRedEyeRemoval(doRedEyeRemoval);
        if (status == NO_ERROR) {
            LOG1("Changed: %s -> %s", CameraParameters::KEY_RED_EYE_MODE, newRedEye);
        }
    }
    return status;
}

status_t ControlThread::processStaticParameters(const CameraParameters *oldParams,
        CameraParameters *newParams)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    bool previewFormatChanged = false;
    float previewAspectRatio = 0.0f;
    float videoAspectRatio = 0.0f;
    Vector<Size> sizes;
    bool videoMode = isParameterSet(CameraParameters::KEY_RECORDING_HINT) ? true : false;

    int oldWidth, newWidth;
    int oldHeight, newHeight;
    int previewWidth, previewHeight;
    int oldFormat, newFormat;

    // see if preview params have changed
    newParams->getPreviewSize(&newWidth, &newHeight);
    oldParams->getPreviewSize(&oldWidth, &oldHeight);
    newFormat = V4L2Format(newParams->getPreviewFormat());
    oldFormat = V4L2Format(oldParams->getPreviewFormat());
    previewWidth = oldWidth;
    previewHeight = oldHeight;
    if (newWidth != oldWidth || newHeight != oldHeight ||
            oldFormat != newFormat) {
        previewWidth = newWidth;
        previewHeight = newHeight;
        previewAspectRatio = 1.0 * newWidth / newHeight;
        LOG1("Preview size/format is changing: old=%dx%d %s; new=%dx%d %s; ratio=%.3f",
                oldWidth, oldHeight, v4l2Fmt2Str(oldFormat),
                newWidth, newHeight, v4l2Fmt2Str(newFormat),
                previewAspectRatio);
        previewFormatChanged = true;
    } else {
        previewAspectRatio = 1.0 * oldWidth / oldHeight;
        LOG1("Preview size/format is unchanged: old=%dx%d %s; ratio=%.3f",
                oldWidth, oldHeight, v4l2Fmt2Str(oldFormat),
                previewAspectRatio);
    }

    // see if video params have changed
    newParams->getVideoSize(&newWidth, &newHeight);
    oldParams->getVideoSize(&oldWidth, &oldHeight);
    if (newWidth != oldWidth || newHeight != oldHeight) {
        videoAspectRatio = 1.0 * newWidth / newHeight;
        LOG1("Video size is changing: old=%dx%d; new=%dx%d; ratio=%.3f",
                oldWidth, oldHeight,
                newWidth, newHeight,
                videoAspectRatio);
        previewFormatChanged = true;
        /*
         *  Camera client requested a new video size, so make sure that requested
         *  video size matches requested preview size. If it does not, then select
         *  a corresponding preview size to match the aspect ratio with video
         *  aspect ratio. Also, the video size must be at least as preview size
         */
        if (fabsf(videoAspectRatio - previewAspectRatio) > ASPECT_TOLERANCE) {
            LOGW("Requested video (%dx%d) aspect ratio does not match preview \
                 (%dx%d) aspect ratio! The preview will be stretched!",
                    newWidth, newHeight,
                    previewWidth, previewHeight);
        }
    } else {
        videoAspectRatio = 1.0 * oldWidth / oldHeight;
        LOG1("Video size is unchanged: old=%dx%d; ratio=%.3f",
                oldWidth, oldHeight,
                videoAspectRatio);
        /*
         *  Camera client did not specify any video size, so make sure that
         *  requested preview size matches our default video size. If it does
         *  not, then select a corresponding video size to match the aspect
         *  ratio with preview aspect ratio.
         */
        if (fabsf(videoAspectRatio - previewAspectRatio) > ASPECT_TOLERANCE) {
            LOG1("Our video (%dx%d) aspect ratio does not match preview (%dx%d) aspect ratio!",
                    newWidth, newHeight,
                    previewWidth, previewHeight);
            newParams->getSupportedVideoSizes(sizes);
            for (size_t i = 0; i < sizes.size(); i++) {
                float thisSizeAspectRatio = 1.0 * sizes[i].width / sizes[i].height;
                if (fabsf(thisSizeAspectRatio - previewAspectRatio) <= ASPECT_TOLERANCE) {
                    if (sizes[i].width < previewWidth || sizes[i].height < previewHeight) {
                        // This video size is smaller than preview, can't use it
                        continue;
                    }
                    newWidth = sizes[i].width;
                    newHeight = sizes[i].height;
                    LOG1("Forcing video to %dx%d to match preview aspect ratio!", newWidth, newHeight);
                    newParams->setVideoSize(newWidth, newHeight);
                    break;
                }
            }
        }
    }

    // if preview is running and static params have changed, then we need
    // to stop, reconfigure, and restart the isp and all threads.
    if (previewFormatChanged) {
        switch (mState) {
        case STATE_PREVIEW_VIDEO:
        case STATE_PREVIEW_STILL:
            status = restartPreview(videoMode);
            break;
        case STATE_STOPPED:
            break;
        default:
            LOGE("formats can only be changed while in preview or stop states");
            break;
        };
    }

    return status;
}

status_t ControlThread::handleMessageSetParameters(MessageSetParameters *msg)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    CameraParameters newParams;
    CameraParameters oldParams = mParameters;
    String8 str_params(msg->params);
    newParams.unflatten(str_params);

    // Workaround: The camera firmware doesn't support preview dimensions that
    // are bigger than video dimensions. If a single preview dimension is larger
    // than the video dimension then the FW will downscale the preview resolution
    // to that of the video resolution.
    if (mState == STATE_PREVIEW_VIDEO || mState == STATE_RECORDING) {

        int pWidth, pHeight;
        int vWidth, vHeight;

        newParams.getPreviewSize(&pWidth, &pHeight);
        newParams.getVideoSize(&vWidth, &vHeight);
        if (vWidth < pWidth || vHeight < pHeight) {
            LOGW("Warning: Video dimension(s) is smaller than preview dimension(s). "
                    "Overriding preview resolution to video resolution [%d, %d] --> [%d, %d]",
                    pWidth, pHeight, vWidth, vHeight);
            newParams.setPreviewSize(vWidth, vHeight);
        }
    }

    // print all old and new params for comparison (debug)
    LOG1("----------BEGIN OLD PARAMS----------");
    mParameters.dump();
    LOG1("---------- END OLD PARAMS ----------");
    LOG1("----------BEGIN NEW PARAMS----------");
    newParams.dump();
    LOG1("---------- END NEW PARAMS ----------");

    status = validateParameters(&newParams);
    if (status != NO_ERROR)
        goto exit;

    mParameters = newParams;

    // Take care of parameters that need to be set while the ISP is stopped
    status = processStaticParameters(&oldParams, &newParams);
    if (status != NO_ERROR)
        goto exit;

    // Take care of parameters that can be set while ISP is running
    status = processDynamicParameters(&oldParams, &newParams);
    if (status != NO_ERROR)
        goto exit;

    mParameters = newParams;

exit:
    // return status and unblock message sender
    mMessageQueue.reply(MESSAGE_ID_SET_PARAMETERS, status);
    return status;
}

status_t ControlThread::handleMessageGetParameters(MessageGetParameters *msg)
{
    status_t status = BAD_VALUE;

    if (msg->params) {
        // let app know if we support zoom in the preview mode indicated
        bool videoMode = isParameterSet(CameraParameters::KEY_RECORDING_HINT) ? true : false;
        AtomMode mode = videoMode ? MODE_VIDEO : MODE_PREVIEW;
        mISP->getZoomRatios(mode, &mParameters);

        String8 params = mParameters.flatten();
        int len = params.length();
        *msg->params = strndup(params.string(), sizeof(char) * len);
        status = NO_ERROR;
    }
    mMessageQueue.reply(MESSAGE_ID_GET_PARAMETERS, status);
    return status;
}
status_t ControlThread::handleMessageCommand(MessageCommand* msg)
{
    status_t status = BAD_VALUE;
    switch (msg->cmd_id)
    {
    case CAMERA_CMD_START_FACE_DETECTION:
        status = startFaceDetection();
        break;
    case CAMERA_CMD_STOP_FACE_DETECTION:
        status = stopFaceDetection();
        break;
    default:
        break;
    }
    return status;
}
/**
 * From Android API:
 * Starts the face detection. This should be called after preview is started.
 * The camera will notify Camera.FaceDetectionListener
 *  of the detected faces in the preview frame. The detected faces may be the same as
 *  the previous ones.
 *
 *  Applications should call stopFaceDetection() to stop the face detection.
 *
 *  This method is supported if getMaxNumDetectedFaces() returns a number larger than 0.
 *  If the face detection has started, apps should not call this again.
 *  When the face detection is running, setWhiteBalance(String), setFocusAreas(List),
 *  and setMeteringAreas(List) have no effect.
 *  The camera uses the detected faces to do auto-white balance, auto exposure, and autofocus.
 *
 *  If the apps call autoFocus(AutoFocusCallback), the camera will stop sending face callbacks.
 *
 *  The last face callback indicates the areas used to do autofocus.
 *  After focus completes, face detection will resume sending face callbacks.
 *
 *  If the apps call cancelAutoFocus(), the face callbacks will also resume.
 *
 *  After calling takePicture(Camera.ShutterCallback, Camera.PictureCallback, Camera.PictureCallback)
 *  or stopPreview(), and then resuming preview with startPreview(),
 *  the apps should call this method again to resume face detection.
 *
 */
status_t ControlThread::startFaceDetection()
{
    LOG2("@%s", __FUNCTION__);
    if (mState == STATE_STOPPED || mFaceDetectionActive) {
        return INVALID_OPERATION;
    }
    if (m_pFaceDetector != 0) {
        m_pFaceDetector->start();
        mFaceDetectionActive = true;
        enableMsgType(CAMERA_MSG_PREVIEW_METADATA);
        return NO_ERROR;
    } else{
        return INVALID_OPERATION;
    }
}

status_t ControlThread::stopFaceDetection(bool wait)
{
    LOG2("@%s", __FUNCTION__);
    if( !mFaceDetectionActive )
        return NO_ERROR;
    mFaceDetectionActive = false;
    disableMsgType(CAMERA_MSG_PREVIEW_METADATA);
    if (m_pFaceDetector != 0) {
        m_pFaceDetector->stop(wait);
        return NO_ERROR;
    } else{
        return INVALID_OPERATION;
    }
}

status_t ControlThread::waitForAndExecuteMessage()
{
    LOG2("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    Message msg;
    mMessageQueue.receive(&msg);

    switch (msg.id) {

        case MESSAGE_ID_EXIT:
            status = handleMessageExit();
            break;

        case MESSAGE_ID_START_PREVIEW:
            status = handleMessageStartPreview();
            break;

        case MESSAGE_ID_STOP_PREVIEW:
            status = handleMessageStopPreview();
            break;

        case MESSAGE_ID_START_RECORDING:
            status = handleMessageStartRecording();
            break;

        case MESSAGE_ID_STOP_RECORDING:
            status = handleMessageStopRecording();
            break;
        case MESSAGE_ID_RELEASE_PREVIEW_FRAME:
            status = handleMessageReleasePreviewFrame(
                &msg.data.releasePreviewFrame);
            break;
        case MESSAGE_ID_TAKE_PICTURE:
            status = handleMessageTakePicture();
            break;

        case MESSAGE_ID_CANCEL_PICTURE:
            status = handleMessageCancelPicture();
            break;

        case MESSAGE_ID_AUTO_FOCUS:
            status = handleMessageAutoFocus();
            break;

        case MESSAGE_ID_CANCEL_AUTO_FOCUS:
            status = handleMessageCancelAutoFocus();
            break;

        case MESSAGE_ID_RELEASE_RECORDING_FRAME:
            status = handleMessageReleaseRecordingFrame(&msg.data.releaseRecordingFrame);
            break;

        case MESSAGE_ID_PREVIEW_DONE:
            status = handleMessagePreviewDone(&msg.data.previewDone);
            break;

        case MESSAGE_ID_PICTURE_DONE:
            status = handleMessagePictureDone(&msg.data.pictureDone);
            break;

        case MESSAGE_ID_REDEYE_REMOVAL_DONE:
            status = handleMessageRedEyeRemovalDone(&msg.data.redEyeRemovalDone);
            break;

        case MESSAGE_ID_AUTO_FOCUS_DONE:
            status = handleMessageAutoFocusDone();
            break;

        case MESSAGE_ID_SET_PARAMETERS:
            status = handleMessageSetParameters(&msg.data.setParameters);
            break;

        case MESSAGE_ID_GET_PARAMETERS:
            status = handleMessageGetParameters(&msg.data.getParameters);
            break;
        case MESSAGE_ID_COMMAND:
            status = handleMessageCommand(&msg.data.command);
            break;
        default:
            LOGE("Invalid message");
            status = BAD_VALUE;
            break;
    };

    if (status != NO_ERROR)
        LOGE("Error handling message: %d", (int) msg.id);
    return status;
}

AtomBuffer* ControlThread::findRecordingBuffer(void *findMe)
{
    // This is a small list, so incremental search is not an issue right now
    if (mCoupledBuffers) {
        for (int i = 0; i < mNumBuffers; i++) {
            if (mCoupledBuffers[i].recordingBuff.buff &&
                    mCoupledBuffers[i].recordingBuff.buff->data == findMe)
                return &mCoupledBuffers[i].recordingBuff;
        }
    }
    return NULL;
}

status_t ControlThread::dequeuePreview()
{
    LOG2("@%s", __FUNCTION__);
    AtomBuffer buff;
    status_t status = NO_ERROR;

    status = mISP->getPreviewFrame(&buff);
    if (status == NO_ERROR) {
        if (mState == STATE_PREVIEW_VIDEO || mState == STATE_RECORDING) {
            mCoupledBuffers[buff.id].previewBuff = buff;
            mCoupledBuffers[buff.id].previewBuffReturned = false;
        }
        if (mAAA->is3ASupported()) {
            status = m3AThread->newFrame();
            if (status != NO_ERROR)
                LOGW("Error notifying new frame to 3A thread!");
        }
        status = mPreviewThread->preview(&buff);
        if (status != NO_ERROR)
            LOGE("Error sending buffer to preview thread");
    } else {
        LOGE("Error gettting preview frame from ISP");
    }
    return status;
}

status_t ControlThread::dequeueRecording()
{
    LOG2("@%s", __FUNCTION__);
    AtomBuffer buff;
    nsecs_t timestamp;
    status_t status = NO_ERROR;

    status = mISP->getRecordingFrame(&buff, &timestamp);
    if (status == NO_ERROR) {
        mCoupledBuffers[buff.id].recordingBuff = buff;
        mCoupledBuffers[buff.id].recordingBuffReturned = false;
        mLastRecordingBuffIndex = buff.id;
        // See if recording has started.
        // If it has, process the buffer
        // If it hasn't, return the buffer to the driver
        if (mState == STATE_RECORDING) {
            mVideoThread->video(&buff, timestamp);
        } else {
            mCoupledBuffers[buff.id].recordingBuffReturned = true;
        }
    } else {
        LOGE("Error: getting recording from isp\n");
    }

    return status;
}

bool ControlThread::recordingBSEncoderEnabled()
{
    return mBSInstance->isBufferSharingModeEnabled();
}

status_t ControlThread::recordingBSEnable()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mBSInstance->sourceRequestToEnableSharingMode() != BS_SUCCESS) {
        LOGE("error requesting to enable buffer share mode");
        status = UNKNOWN_ERROR;
    } else {
        mBSState = BS_STATE_ENABLE;
    }

    return status;
}

status_t ControlThread::recordingBSDisable()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mBSInstance->sourceRequestToDisableSharingMode() != BS_SUCCESS) {
        LOGE("error requesting to disable buffer share mode");
        status = UNKNOWN_ERROR;
    } else {
        mBSState = BS_STATE_DISABLED;
    }

    return status;
}

status_t ControlThread::recordingBSSet()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    SharedBufferType *buffers;
    int numBuffers = 0;

    if (mBSInstance->sourceEnterSharingMode() != BS_SUCCESS) {
        LOGE("error entering buffer share mode");
        status = UNKNOWN_ERROR;
        goto failGeneric;
    }

    if (!mBSInstance->isBufferSharingModeSet()) {
        LOGE("sharing is expected to be set but isn't");
        status = UNKNOWN_ERROR;
        goto failGeneric;
    }

    if (mBSInstance->sourceGetSharedBuffer(NULL, &numBuffers) != BS_SUCCESS) {
        LOGE("error getting number of shared buffers");
        status = UNKNOWN_ERROR;
        goto failGeneric;
    }

    buffers = new SharedBufferType[numBuffers];
    if (buffers == NULL) {
        LOGE("error allocating sharedbuffer array");
        goto failGeneric;
    }

    if (mBSInstance->sourceGetSharedBuffer(buffers, NULL) != BS_SUCCESS) {
        LOGE("error getting shared buffers");
        status = UNKNOWN_ERROR;
        goto failAlloc;
    }

    for (int i = 0; i < numBuffers; i++)
        LOG1("shared buffer[%d]=%p", i, buffers[i].pointer);

    status = stopPreviewCore();
    if (status != NO_ERROR) {
        LOGE("error stopping preview for buffer sharing");
        goto failAlloc;
    }

    status = mISP->setRecordingBuffers(buffers, numBuffers);
    if (status != NO_ERROR) {
        LOGE("error setting recording buffers");
        goto failAlloc;
    }

    status = startPreviewCore(true);
    if (status != NO_ERROR) {
        LOGE("error restarting preview for buffer sharing");
        goto failStart;
    }

    mState = STATE_RECORDING;
    mBSState = BS_STATE_SET;
    return status;

failStart:
    mISP->unsetRecordingBuffers();
failAlloc:
    delete [] buffers;
failGeneric:
    return status;
}

status_t ControlThread::recordingBSUnset()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (mBSInstance->sourceExitSharingMode() != BS_SUCCESS) {
        LOGE("error exiting buffer share mode");
        return UNKNOWN_ERROR;
    }

    status = stopPreviewCore();
    if (status != NO_ERROR) {
        LOGE("error stopping preview for buffer sharing");
        return status;
    }

    mISP->unsetRecordingBuffers();

    status = startPreviewCore(true);
    if (status != NO_ERROR) {
        LOGE("error starting preview for buffer sharing");
        return status;
    }

    mState = STATE_RECORDING;
    mBSState = BS_STATE_UNSET;

    return status;
}

bool ControlThread::recordingBSEncoderSet()
{
    return mBSInstance->isBufferSharingModeSet();
}

status_t ControlThread::recordingBSHandshake()
{
    LOG2("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // see comments for enum BSState
    switch (mBSState) {

        // if encoder has enabled BS, then set BS
        case BS_STATE_ENABLE:
            if (recordingBSEncoderEnabled()) {
                status = recordingBSSet();
                if (status != NO_ERROR)
                    LOGE("error setting buffer sharing");
            }
            break;

        // if encoder has set BS, the go to steady state
        // time to start sending buffers!
        case BS_STATE_SET:
            if (recordingBSEncoderSet()) {
                mBSState = BS_STATE_STEADY;
            }
            break;

        // if encoder has unset BS, then we need to unset BS
        // this essentially means that the encoder was torn down
        // via stopRecording, and the app is about to call stopRecording
        // on the camera HAL
        case BS_STATE_STEADY:
            if (!recordingBSEncoderSet()) {
                status = recordingBSUnset();
                if (status != NO_ERROR)
                    LOGE("error unsetting buffer sharing");
            }
            break;

        case BS_STATE_UNSET:
        case BS_STATE_DISABLED:
            // do nothing
            break;

        default:
            LOGE("unexpected bs state %d", (int) mBSState);
    };

    return status;
}

bool ControlThread::threadLoop()
{
    LOG2("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    mThreadRunning = true;
    while (mThreadRunning) {

        switch (mState) {

        case STATE_STOPPED:
            LOG2("In STATE_STOPPED...");
            // in the stop state all we do is wait for messages
            status = waitForAndExecuteMessage();
            break;

        case STATE_CAPTURE:
            LOG2("In STATE_CAPTURE...");
            // message queue always has priority over getting data from the
            // isp driver no matter what state we are in
            if (!mMessageQueue.isEmpty()) {
                status = waitForAndExecuteMessage();
            } else {
                // make sure ISP has data before we ask for some
                if (mISP->dataAvailable())
                    status = handleMessageTakePicture(false);
                else
                    status = waitForAndExecuteMessage();
            }
            break;

        case STATE_PREVIEW_STILL:
            LOG2("In STATE_PREVIEW_STILL...");
            // message queue always has priority over getting data from the
            // isp driver no matter what state we are in
            if (!mMessageQueue.isEmpty()) {
                status = waitForAndExecuteMessage();
            } else {
                // make sure ISP has data before we ask for some
                if (mISP->dataAvailable())
                    status = dequeuePreview();
                else
                    status = waitForAndExecuteMessage();
            }
            break;

        case STATE_PREVIEW_VIDEO:
        case STATE_RECORDING:
            LOG2("In %s...", mState == STATE_PREVIEW_VIDEO ? "STATE_PREVIEW_VIDEO" : "STATE_RECORDING");
            // message queue always has priority over getting data from the
            // isp driver no matter what state we are in
            if (!mMessageQueue.isEmpty()) {
                status = waitForAndExecuteMessage();
            } else {

                if (mState == STATE_RECORDING)
                    recordingBSHandshake();

                // make sure ISP has data before we ask for some
                if (mISP->dataAvailable()) {
                    status = dequeueRecording();
                    if (status == NO_ERROR)
                        status = dequeuePreview();
                } else {
                    status = waitForAndExecuteMessage();
                }
            }
            break;

        default:
            break;
        };
    }

    return false;
}

status_t ControlThread::requestExitAndWait()
{
    LOG1("@%s", __FUNCTION__);
    Message msg;
    msg.id = MESSAGE_ID_EXIT;

    // tell thread to exit
    // send message asynchronously
    mMessageQueue.send(&msg);

    // propagate call to base class
    return Thread::requestExitAndWait();
}

} // namespace android

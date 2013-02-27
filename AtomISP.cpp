/*
 * Copyright (C) 2011 The Android Open Source Project
 * Copyright (C) 2011,2012,2013 Intel Corporation
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
#define LOG_TAG "Camera_ISP"

#include "LogHelper.h"
#include "AtomISP.h"
#include "Callbacks.h"
#include "ColorConverter.h"
#include "PlatformData.h"
#include "FeatureData.h"
#include "IntelParameters.h"
#include "PanoramaThread.h"
#include "CameraDump.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include "PerformanceTraces.h"

#define CLEAR(x) memset (&(x), 0, sizeof (x))
#define PAGE_ALIGN(x) ((x + 0xfff) & 0xfffff000)
#define main_fd video_fds[V4L2_MAIN_DEVICE]

#define DEFAULT_SENSOR_FPS      15.0

#define RESOLUTION_14MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1920x1080,2048x1536,2560x1920,3264x1836,3264x2448,3648x2736,4096x3072,4352x3264"

#define RESOLUTION_13MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1920x1080,2048x1536,2560x1920,3264x1836,3264x2448,3648x2736,4096x3072,4192x3104"

#define RESOLUTION_8MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1920x1080,2048x1536,2560x1920,3264x1836,3264x2448"

#define RESOLUTION_5MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1920x1080,2048x1536,2560x1920"

#define RESOLUTION_3MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1280x960,1536x864,1600x1200,1920x1080,2048x1152,2048x1536"

#define RESOLUTION_1080P_TABLE   \
        "320x240,640x480,1024x768,1280x720,1920x1080"

#define RESOLUTION_2MP_TABLE   \
        "320x240,640x480,1024x768,1280x720,1600x900,1600x1200"

#define RESOLUTION_1_3MP_TABLE   \
        "320x240,640x480,1280x720,1280x960"

#define RESOLUTION_720P_TABLE   \
        "320x240,640x480,1280x720"

#define RESOLUTION_VGA_TABLE   \
        "320x240,640x480"

#define MAX_BACK_CAMERA_PREVIEW_WIDTH   1280
#define MAX_BACK_CAMERA_PREVIEW_HEIGHT  720
#define MAX_BACK_CAMERA_VIDEO_WIDTH   1920
#define MAX_BACK_CAMERA_VIDEO_HEIGHT  1088

#define MAX_FRONT_CAMERA_PREVIEW_WIDTH  1280
#define MAX_FRONT_CAMERA_PREVIEW_HEIGHT 720
#define MAX_FRONT_CAMERA_SNAPSHOT_WIDTH 1920
#define MAX_FRONT_CAMERA_SNAPSHOT_HEIGHT    1080
#define MAX_FRONT_CAMERA_VIDEO_WIDTH   1920
#define MAX_FRONT_CAMERA_VIDEO_HEIGHT  1088

#define MAX_FILE_INJECTION_SNAPSHOT_WIDTH    3264
#define MAX_FILE_INJECTION_SNAPSHOT_HEIGHT   2448
#define MAX_FILE_INJECTION_PREVIEW_WIDTH     1280
#define MAX_FILE_INJECTION_PREVIEW_HEIGHT    720
#define MAX_FILE_INJECTION_RECORDING_WIDTH   1920
#define MAX_FILE_INJECTION_RECORDING_HEIGHT  1088

#define MAX_ZOOM_LEVEL          150     // How many levels we have from 1x -> max zoom
#define MIN_ZOOM_LEVEL          0
#define MIN_SUPPORT_ZOOM        100     // Support 1x at least
#define MAX_SUPPORT_ZOOM        1600    // Support upto 16x and should not bigger than 99x
#define ZOOM_RATIO              100     // Conversion between zoom to really zoom effect

#define INTEL_FILE_INJECT_CAMERA_ID 2

#define ATOMISP_PREVIEW_POLL_TIMEOUT 1000
#define ATOMISP_GETFRAME_RETRY_COUNT 5  // Times to retry poll/dqbuf in case of error
#define ATOMISP_GETFRAME_STARVING_WAIT 200000 // Time to usleep between retry's when stream is starving from buffers.

#define FRAME_SYNC_POLL_TIMEOUT 500

/**
 * Checks whether 'device' is a valid atomisp V4L2 device node
 */
#define VALID_DEVICE(device, x) \
    if ((device < V4L2_MAIN_DEVICE || device > mConfigLastDevice) \
            && device != V4L2_ISP_SUBDEV) { \
        LOGE("%s: Wrong device %d (last %d)", __FUNCTION__, device, mConfigLastDevice); \
        return x; \
    }

namespace android {

////////////////////////////////////////////////////////////////////
//                          STATIC DATA
////////////////////////////////////////////////////////////////////

static const char *dev_name_array[] = {"/dev/video0",
                                       "/dev/video1",
                                       "/dev/video2",
                                       "/dev/video3"};

AtomISP::cameraInfo AtomISP::sCamInfo[MAX_CAMERA_NODES];

static const char *resolution_tables[] = {
    RESOLUTION_VGA_TABLE,
    RESOLUTION_720P_TABLE,
    RESOLUTION_1_3MP_TABLE,
    RESOLUTION_2MP_TABLE,
    RESOLUTION_1080P_TABLE,
    RESOLUTION_3MP_TABLE,
    RESOLUTION_5MP_TABLE,
    RESOLUTION_8MP_TABLE,
    RESOLUTION_13MP_TABLE,
    RESOLUTION_14MP_TABLE
};

// Generated the string like "100,110,120, ...,1580,1590,1600"
// The string is determined by MAX_ZOOM_LEVEL and MAX_SUPPORT_ZOOM
static void computeZoomRatios(char *zoom_ratio, int max_count){

    //set up zoom ratio according to MAX_ZOOM_LEVEL
    int zoom_step = (MAX_SUPPORT_ZOOM - MIN_SUPPORT_ZOOM)/MAX_ZOOM_LEVEL;
    int ratio = MIN_SUPPORT_ZOOM;
    int pos = 0;
    //Get zoom from MIN_SUPPORT_ZOOM to MAX_SUPPORT_ZOOM
    while((ratio <= MAX_SUPPORT_ZOOM) && (pos < max_count)){
        sprintf(zoom_ratio + pos,"%d,",ratio);
        if (ratio < 1000)
            pos += 4;
        else
            pos += 5;
        ratio += zoom_step;
    }

    //Overwrite the last ',' with '\0'
    if (pos > 0)
        *(zoom_ratio + pos -1 ) = '\0';
}


////////////////////////////////////////////////////////////////////
//                          PUBLIC METHODS
////////////////////////////////////////////////////////////////////

AtomISP::AtomISP(const sp<CameraConf>& cfg) :
     mPreviewStreamSource("PreviewStreamSource", this)
    ,mFrameSyncSource("FrameSyncSource", this)
    ,mCameraConf(cfg)
    ,mMode(MODE_NONE)
    ,mCallbacks(Callbacks::getInstance())
    ,mNumBuffers(PlatformData::getRecordingBufNum())
    ,mNumPreviewBuffers(PlatformData::getRecordingBufNum())
    ,mPreviewBuffers(NULL)
    ,mRecordingBuffers(NULL)
    ,mSwapRecordingDevice(false)
    ,mRecordingDeviceSwapped(false)
    ,mClientSnapshotBuffers(NULL)
    ,mUsingClientSnapshotBuffers(false)
    ,mStoreMetaDataInBuffers(false)
    ,mNumPreviewBuffersQueued(0)
    ,mNumRecordingBuffersQueued(0)
    ,mNumCapturegBuffersQueued(0)
    ,mFlashTorchSetting(0)
    ,mContCaptPrepared(false)
    ,mConfigSnapshotPreviewDevice(V4L2_MAIN_DEVICE)
    ,mConfigLastDevice(V4L2_PREVIEW_DEVICE)
    ,mPreviewDevice(V4L2_MAIN_DEVICE)
    ,mRecordingDevice(V4L2_MAIN_DEVICE)
    ,mSessionId(0)
    ,mLowLight(false)
    ,mXnr(0)
    ,mZoomRatios(NULL)
    ,mRawDataDumpSize(0)
    ,mFrameSyncRequested(0)
    ,mFrameSyncEnabled(false)
    ,mColorEffect(V4L2_COLORFX_NONE)
    ,mObserverManager()
    ,mPublicAeMode(CAM_AE_MODE_AUTO)
    ,mPublicAfMode(CAM_AF_MODE_AUTO)
{
    LOG1("@%s", __FUNCTION__);

    for(int i = 0; i < V4L2_MAX_DEVICE_COUNT; i++) {
        video_fds[i] = -1;
        mDevices[i].state = DEVICE_CLOSED;
    }

    CLEAR(mSnapshotBuffers);
    CLEAR(mPostviewBuffers);
    CLEAR(mContCaptConfig);
}

status_t AtomISP::initDevice()
{
    status_t status = NO_ERROR;

    initDriverVersion();

    // Open the main device first, this device will remain open during object life span
    // and will be closed in the object destructor
    int ret = openDevice(V4L2_MAIN_DEVICE);
    if (ret < 0) {
        LOGE("Failed to open first device!");
        return NO_INIT;
    }
    PERFORMANCE_TRACES_BREAKDOWN_STEP("Open_Main_Device");

    initFileInject();

    // Select the input port to use
    status = initCameraInput();
    if (status != NO_ERROR) {
        LOGE("Unable to initialize camera input %d", mCameraConf->cameraId());
        return NO_INIT;
    }

    mSensorType = PlatformData::sensorType(getCurrentCameraId());
    LOG1("Sensor type detected: %s", (mSensorType == SENSOR_TYPE_RAW)?"RAW":"SOC");
    return status;
}

/**
 * Closes the main device
 *
 * This is specifically provided for error recovery and
 * expected to be called after AtomISP::stop(), where the
 * rest of the devices are already closed and associated
 * buffers are all freed.
 *
 * TODO: protect AtomISP API agains uninitialized state and
 *       support direct uninit regardless of the state.
 */
void AtomISP::deInitDevice()
{
    closeDevice(V4L2_MAIN_DEVICE);
}

/**
 * Checks if main device is open
 */
bool AtomISP::isDeviceInitialized() const
{
    return (video_fds[V4L2_MAIN_DEVICE] >= 0);
}

status_t AtomISP::init()
{
    status_t status = NO_ERROR;

    status = initDevice();
    if (status != NO_ERROR) {
        return NO_INIT;
    }

    mConfig.fps = 30;
    mConfig.num_snapshot = 1;
    mConfig.zoom = 0;

    if (selectCameraSensor() != NO_ERROR) {
       LOGE("Could not select camera: %s", mCameraInput->name);
       return NO_INIT;
    }
    PERFORMANCE_TRACES_BREAKDOWN_STEP("Init_3A");

    initFrameConfig();

    // Initialize the frame sizes
    setPreviewFrameFormat(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT, V4L2_PIX_FMT_NV12);
    setPostviewFrameFormat(RESOLUTION_POSTVIEW_WIDTH, RESOLUTION_POSTVIEW_HEIGHT, V4L2_PIX_FMT_NV12);
    setSnapshotFrameFormat(RESOLUTION_5MP_WIDTH, RESOLUTION_5MP_HEIGHT, V4L2_PIX_FMT_NV12);
    setVideoFrameFormat(RESOLUTION_VGA_WIDTH, RESOLUTION_VGA_HEIGHT, V4L2_PIX_FMT_NV12);

    /*
       Zoom is describled as 100, 200, each level has less memory than 5 bytes
       We don't support zoom bigger than 9999
       The last byte is used to store '\0'
     */
    static const int zoomBytes = MAX_ZOOM_LEVEL * 5 + 1;
    mZoomRatios = new char[zoomBytes];
    computeZoomRatios(mZoomRatios, zoomBytes);

    return status;
}

int AtomISP::getPrimaryCameraIndex(void) const
{
    int res = 0;
    int items = sizeof(sCamInfo) / sizeof(cameraInfo);
    for (int i = 0; i < items; i++) {
        if (sCamInfo[i].port == ATOMISP_CAMERA_PORT_PRIMARY) {
            res = i;
            break;
        }
    }
    return res;
}

int AtomISP::getCurrentCameraId(void)
{
    if (mCameraInput->androidCameraId < 0)
        LOGE("%s: Camera ID is wrong : %d", __func__, mCameraInput->androidCameraId);

    return mCameraInput->androidCameraId;
}

/**
 * Convert zoom value to zoom ratio
 *
 * @param zoomValue zoom value to convert to zoom ratio
 *
 * @return zoom ratio multiplied by 100
 */
int AtomISP::zoomRatio(int zoomValue) {
    if (zoomValue > MAX_ZOOM_LEVEL) {
        LOGE("Too big zoom value");
        return BAD_VALUE;
    }

    int zoomStep = (MAX_SUPPORT_ZOOM - MIN_SUPPORT_ZOOM) / MAX_ZOOM_LEVEL;
    return MIN_SUPPORT_ZOOM + zoomValue * zoomStep;
}

/**
 * Detects which AtomISP kernel driver is used in the system
 *
 * Only to be called from 2nd stage contructor AtomISP::init().
 */
void AtomISP::initDriverVersion(void)
{
    struct stat buf;

    /*
     * This version of AtomISP supports two kernel driver variants:
     *
     *  1) driver that uses four distinct /dev/video device nodes and
     *     has a separate device node for preview, and
     *  2) driver that uses three /dev/video device nodes and uses
     *     the first/main device both for snapshot preview and actual
     *     main capture
     */
    int res = stat("/dev/video3", &buf);
    if (!res) {
        LOGD("Kernel with separate preview device node detected");

        mConfigSnapshotPreviewDevice = V4L2_PREVIEW_DEVICE;
        mConfigRecordingPreviewDevice = V4L2_PREVIEW_DEVICE;
        mConfigLastDevice = 3;
    }
    else {
        LOGD("Kernel with multiplexed preview and main devices detected");

        mConfigSnapshotPreviewDevice = V4L2_MAIN_DEVICE;
        mConfigRecordingPreviewDevice = V4L2_LEGACY_VIDEO_PREVIEW_DEVICE;
        mConfigLastDevice = 2;
    }
}

/**
 * Only to be called from 2nd stage contructor AtomISP::init().
 */
void AtomISP::initFrameConfig()
{
    if (mIsFileInject) {
        mConfig.snapshot.maxWidth = MAX_FILE_INJECTION_SNAPSHOT_WIDTH;
        mConfig.snapshot.maxHeight = MAX_FILE_INJECTION_SNAPSHOT_HEIGHT;
        mConfig.preview.maxWidth = MAX_FILE_INJECTION_PREVIEW_WIDTH;
        mConfig.preview.maxHeight = MAX_FILE_INJECTION_PREVIEW_HEIGHT;
        mConfig.recording.maxWidth = MAX_FILE_INJECTION_RECORDING_WIDTH;
        mConfig.recording.maxHeight = MAX_FILE_INJECTION_RECORDING_HEIGHT;
    }
    else {
        int width, height;
        PlatformData::maxSnapshotSize(mCameraInput->androidCameraId, &width, &height);
        mConfig.snapshot.maxWidth  = width;
        mConfig.snapshot.maxHeight = height;
	/* workround to support two main sensor for vv - need to removed when one main sensor used */
        if (strstr(mCameraInput->name, "imx175")) {
           mConfig.snapshot.maxWidth  = RESOLUTION_8MP_WIDTH;
           mConfig.snapshot.maxHeight = RESOLUTION_8MP_HEIGHT;
        }
        if (strstr(mCameraInput->name, "imx135")) {
           mConfig.snapshot.maxWidth  = RESOLUTION_13MP_WIDTH;
           mConfig.snapshot.maxHeight = RESOLUTION_13MP_HEIGHT;
        }
    }

    if (mConfig.snapshot.maxWidth >= RESOLUTION_720P_WIDTH
        && mConfig.snapshot.maxHeight >= RESOLUTION_720P_HEIGHT) {
        mConfig.preview.maxWidth = RESOLUTION_720P_WIDTH;
        mConfig.preview.maxHeight = RESOLUTION_720P_HEIGHT;
    } else {
        mConfig.preview.maxWidth = mConfig.snapshot.maxWidth;
        mConfig.preview.maxHeight =  mConfig.snapshot.maxHeight;
    }

    if (mConfig.snapshot.maxWidth >= RESOLUTION_1080P_WIDTH
        && mConfig.snapshot.maxHeight >= RESOLUTION_1080P_HEIGHT) {
        mConfig.recording.maxWidth = RESOLUTION_1080P_WIDTH;
        mConfig.recording.maxHeight = RESOLUTION_1080P_HEIGHT;
    } else {
        mConfig.recording.maxWidth = mConfig.snapshot.maxWidth;
        mConfig.recording.maxHeight = mConfig.snapshot.maxHeight;
    }
}

/**
 * Maps the requested 'cameraId' to a V4L2 input.
 *
 * Only to be called from constructor
 * The cameraId  is passed to the HAL during creation as is currently stored in
 * the Camera Configuration Class (CPF Store)
 * This CameraID is used to identify a particular camera, it maps always 0 to back
 * camera and 1 to front whereas the index in the sCamInfo is filled from V4L2
 * The order how front and back camera are returned
 * may be different. This Android camera id will be used
 * to select parameters from back or front camera
 */
status_t AtomISP::initCameraInput()
{
    status_t status = NO_INIT;
    size_t numCameras = setupCameraInfo();
    mCameraInput = 0;

    for (size_t i = 0; i < numCameras; i++) {

        // BACK camera -> AtomISP/V4L2 primary port
        // FRONT camera -> AomISP/V4L2 secondary port

        if ((PlatformData::cameraFacing(mCameraConf->cameraId()) == CAMERA_FACING_BACK &&
             sCamInfo[i].port == ATOMISP_CAMERA_PORT_PRIMARY) ||
            (PlatformData::cameraFacing(mCameraConf->cameraId()) == CAMERA_FACING_FRONT &&
             sCamInfo[i].port == ATOMISP_CAMERA_PORT_SECONDARY)) {
            mCameraInput = &sCamInfo[i];
            mCameraInput->androidCameraId = mCameraConf->cameraId();
            LOG1("Camera found, v4l2 dev %d, android cameraId %d", i, mCameraConf->cameraId());
            status = NO_ERROR;
            break;
        }
    }

    if (mIsFileInject) {
        LOG1("AtomISP opened with file inject camera id");
        mCameraInput = &sCamInfo[INTEL_FILE_INJECT_CAMERA_ID];
        mFileInject.active = true;
        status = NO_ERROR;
    }

    return status;
}

/**
 * Retrieves the sensor parameters and  CPFStore AIQ configuration
 * Only to be called after AtomISP initialization
 *
 * These parameters are needed for Intel 3A initialization
 * This method is called by AtomAAA during init3A()
 *
 * \param paramsAndCPF pointer to an allocated SensorParams structure
 */
status_t AtomISP::getSensorParams(SensorParams *paramsAndCPF)
{
    status_t status = NO_ERROR;
    const SensorParams *paramFiles;

    if (paramsAndCPF == NULL)
        return BAD_VALUE;

    if (mIsFileInject) {
        int maincam = getPrimaryCameraIndex();
        paramFiles = PlatformData::getSensorParamsFile(sCamInfo[maincam].name);
    } else {
        paramFiles = PlatformData::getSensorParamsFile(mCameraInput->name);
    }

    if (paramFiles == NULL)
        return UNKNOWN_ERROR;

    *paramsAndCPF = *paramFiles;
    if (mCameraConf->aiqConf != 0) {
        paramsAndCPF->cpfData.data = mCameraConf->aiqConf->ptr();
        paramsAndCPF->cpfData.size = mCameraConf->aiqConf->size();
    }
    // We don't need this memory anymore
    mCameraConf->aiqConf.clear();


    return status;
}


/**
 * Only to be called from 2nd stage contructor AtomISP::init().
 */
void AtomISP::initFileInject()
{
    mIsFileInject = (PlatformData::supportsFileInject() == true) && (mCameraConf->cameraId() == INTEL_FILE_INJECT_CAMERA_ID);
    mFileInject.active = false;
}

AtomISP::~AtomISP()
{
    LOG1("@%s", __FUNCTION__);
    /*
     * The destructor is called when the hw_module close mehod is called. The close method is called
     * in general by the camera client when it's done with the camera device, but it is also called by
     * System Server when the camera application crashes. System Server calls close in order to release
     * the camera hardware module. So, if we are not in MODE_NONE, it means that we are in the middle of
     * something when the close function was called. So it's our duty to stop first, then close the
     * camera device.
     */
    if (mMode != MODE_NONE) {
        stop();

        // note: AtomISP allows to stop capture without freeing, so
        //       we need to make sure we free them here.
        //       This is not needed for preview and recording buffers.
        freeSnapshotBuffers();
    }
    closeDevice(V4L2_MAIN_DEVICE);

    if (mZoomRatios)
        delete[] mZoomRatios;
}

void AtomISP::getDefaultParameters(CameraParameters *params, CameraParameters *intel_params)
{
    LOG2("@%s", __FUNCTION__);
    if (!params) {
        LOGE("params is null!");
        return;
    }
    int cameraId = mCameraInput->androidCameraId;
    /**
     * PREVIEW
     */
    params->setPreviewSize(mConfig.preview.width, mConfig.preview.height);
    params->setPreviewFrameRate(30);


    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES, PlatformData::supportedPreviewSize(cameraId));

    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,PlatformData::supportedPreviewFrameRate(cameraId));
    params->set(CameraParameters::KEY_PREVIEW_FPS_RANGE,PlatformData::defaultPreviewFPSRange(cameraId));
    params->set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,PlatformData::supportedPreviewFPSRange(cameraId));

    /**
     * RECORDING
     */
    params->setVideoSize(mConfig.recording.width, mConfig.recording.height);
    params->set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, PlatformData::preferredPreviewSizeForVideo());
    params->set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES, PlatformData::supportedVideoSizes());
    params->set(CameraParameters::KEY_VIDEO_FRAME_FORMAT,
                CameraParameters::PIXEL_FORMAT_YUV420SP);
    if (PlatformData::supportVideoSnapshot())
        params->set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED, CameraParameters::TRUE);
    else
        params->set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED, CameraParameters::FALSE);

    /**
     * SNAPSHOT
     */
    const char *picSizes = getMaxSnapShotResolution();
    params->set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES, picSizes);
    params->setPictureSize(mConfig.snapshot.width, mConfig.snapshot.height);
    params->set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,"320");
    params->set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,"240");
    params->set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES, CAM_RESO_STR(LARGEST_THUMBNAIL_WIDTH,LARGEST_THUMBNAIL_HEIGHT)
                ",240x320,320x180,180x320,160x120,120x160,0x0");

    /**
     * ZOOM
     */
    params->set(CameraParameters::KEY_ZOOM, 0);
    params->set(CameraParameters::KEY_ZOOM_SUPPORTED, CameraParameters::TRUE);

    /**
     * ROTATION
     */
    params->set(CameraParameters::KEY_ROTATION, 0);

    /**
     * FLASH
     */
    if (PlatformData::supportsBackFlash() == true &&
        mCameraInput->port == ATOMISP_CAMERA_PORT_PRIMARY) {

        // For main back camera
        // flash mode option, cts mandates default to be off
        params->set(CameraParameters::KEY_FLASH_MODE, PlatformData::defaultFlashMode(cameraId));
        params->set(CameraParameters::KEY_SUPPORTED_FLASH_MODES, PlatformData::supportedFlashModes(cameraId));
    }

    /**
     * FOCUS
     */

    params->set(CameraParameters::KEY_FOCUS_MODE, PlatformData::defaultFocusMode(cameraId));
    params->set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES, PlatformData::supportedFocusModes(cameraId));

    /**
     * FOCAL LENGTH
     */
    atomisp_makernote_info makerNote;
    getMakerNote(&makerNote);
    float focal_length = ((float)((makerNote.focal_length>>16) & 0xFFFF)) /
        ((float)(makerNote.focal_length & 0xFFFF));
    char focalLength[100] = {0};
    if (snprintf(focalLength, sizeof(focalLength),"%f", focal_length) < 0) {
        LOGE("Could not generate %s string: %s",
            CameraParameters::KEY_FOCAL_LENGTH, strerror(errno));
        return;
    }
    params->set(CameraParameters::KEY_FOCAL_LENGTH,focalLength);

    /**
     * FOCUS DISTANCES
     */
    getFocusDistances(params);

    /**
     * DIGITAL VIDEO STABILIZATION
     */
    if(PlatformData::supportsDVS(cameraId))
    {
        params->set(CameraParameters::KEY_VIDEO_STABILIZATION_SUPPORTED,"true");
        params->set(CameraParameters::KEY_VIDEO_STABILIZATION,"true");
    }

    /**
     * MISCELLANEOUS
     */
    params->set(CameraParameters::KEY_VERTICAL_VIEW_ANGLE,"42.5");
    params->set(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE,"54.8");

    /**
     * OVERLAY
     */
    if (PlatformData::renderPreviewViaOverlay(mCameraInput->androidCameraId)) {
        intel_params->set(IntelCameraParameters::KEY_HW_OVERLAY_RENDERING_SUPPORTED, "true,false");
    } else {
        intel_params->set(IntelCameraParameters::KEY_HW_OVERLAY_RENDERING_SUPPORTED, "false");
    }
    intel_params->set(IntelCameraParameters::KEY_HW_OVERLAY_RENDERING,"false");

    /**
     * flicker mode
     */
    if(mCameraInput->port == ATOMISP_CAMERA_PORT_PRIMARY) {
        params->set(CameraParameters::KEY_ANTIBANDING, "auto");
        params->set(CameraParameters::KEY_SUPPORTED_ANTIBANDING, "off,50hz,60hz,auto");
    } else {
        params->set(CameraParameters::KEY_ANTIBANDING, "50hz");
        params->set(CameraParameters::KEY_SUPPORTED_ANTIBANDING, "50hz,60hz");
    }

    /**
     * XNR/ANR
     */
    if (mSensorType == SENSOR_TYPE_RAW) {
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_XNR, "true,false");
        intel_params->set(IntelCameraParameters::KEY_XNR, CameraParameters::FALSE);
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_ANR, "true,false");
        intel_params->set(IntelCameraParameters::KEY_ANR, CameraParameters::FALSE);
    }

    /**
     * EXPOSURE
     */
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION, PlatformData::supportedDefaultEV(cameraId));
    params->set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION,PlatformData::supportedMaxEV(cameraId));
    params->set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION,PlatformData::supportedMinEV(cameraId));
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP,PlatformData::supportedStepEV(cameraId));

    // No Capture bracketing
    intel_params->set(IntelCameraParameters::KEY_CAPTURE_BRACKET, "none");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_CAPTURE_BRACKET, "none");

    // HDR imaging settings
    intel_params->set(IntelCameraParameters::KEY_HDR_IMAGING, FeatureData::hdrDefault(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_HDR_IMAGING, FeatureData::hdrSupported(cameraId));
    intel_params->set(IntelCameraParameters::KEY_HDR_VIVIDNESS, "none");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_HDR_VIVIDNESS, "none");
    intel_params->set(IntelCameraParameters::KEY_HDR_SHARPENING, "none");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_HDR_SHARPENING, "none");
    intel_params->set(IntelCameraParameters::KEY_HDR_SAVE_ORIGINAL, "off");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_HDR_SAVE_ORIGINAL, "off");

    /**
     * Burst-mode
     */
    // Currently burst support is required only with primary camera.
    // So burst mode is disabled to secondary camera.
    const char* startIndexValues = "0";
    if (mCameraInput->port == ATOMISP_CAMERA_PORT_PRIMARY) {
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_BURST_FPS, PlatformData::supportedBurstFPS(cameraId));
        intel_params->set(IntelCameraParameters::KEY_BURST_LENGTH,"1");
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_BURST_LENGTH, PlatformData::supportedBurstLength(cameraId));

        // Bursts with negative start offset require a RAW sensor.
        if (PlatformData::sensorType(cameraId) ==  SENSOR_TYPE_RAW &&
                PlatformData::supportsContinuousCapture())
            startIndexValues = "-4,-3,-2,-1,0";
    } else {
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_BURST_FPS, "1");
        intel_params->set(IntelCameraParameters::KEY_BURST_LENGTH, "1");
        intel_params->set(IntelCameraParameters::KEY_SUPPORTED_BURST_LENGTH, "1");
    }
    intel_params->set(IntelCameraParameters::KEY_BURST_FPS, "1");
    intel_params->set(IntelCameraParameters::KEY_BURST_START_INDEX, "0");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_BURST_START_INDEX, startIndexValues);

    intel_params->set(IntelCameraParameters::KEY_FILE_INJECT_FILENAME, "off");
    intel_params->set(IntelCameraParameters::KEY_FILE_INJECT_WIDTH, "0");
    intel_params->set(IntelCameraParameters::KEY_FILE_INJECT_HEIGHT, "0");
    intel_params->set(IntelCameraParameters::KEY_FILE_INJECT_BAYER_ORDER, "0");
    intel_params->set(IntelCameraParameters::KEY_FILE_INJECT_FORMAT,"0");

    // raw data format for snapshot
    intel_params->set(IntelCameraParameters::KEY_RAW_DATA_FORMAT, "none");
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_RAW_DATA_FORMATS, "none,yuv,bayer");

    // effect modes
    params->set(CameraParameters::KEY_EFFECT, PlatformData::defaultEffectMode(cameraId));
    params->set(CameraParameters::KEY_SUPPORTED_EFFECTS, PlatformData::supportedEffectModes(cameraId));
    intel_params->set(CameraParameters::KEY_SUPPORTED_EFFECTS, PlatformData::supportedIntelEffectModes(cameraId));
    //awb
    params->set(CameraParameters::KEY_WHITE_BALANCE, PlatformData::defaultAwbMode(cameraId));
    params->set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE, PlatformData::supportedAwbModes(cameraId));
    // scene mode
    params->set(CameraParameters::KEY_SUPPORTED_SCENE_MODES, PlatformData::supportedSceneModes(cameraId));
    params->set(CameraParameters::KEY_SCENE_MODE, PlatformData::defaultSceneMode(cameraId));

    // exposure compensation
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION, PlatformData::supportedDefaultEV(cameraId));
    params->set(CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION, PlatformData::supportedMaxEV(cameraId));
    params->set(CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION, PlatformData::supportedMinEV(cameraId));
    params->set(CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP, PlatformData::supportedStepEV(cameraId));

    // ae metering mode (Intel extension)
    intel_params->set(IntelCameraParameters::KEY_AE_METERING_MODE, PlatformData::defaultAeMetering(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_AE_METERING_MODES, PlatformData::supportedAeMetering(cameraId));

    // manual iso control (Intel extension)
    intel_params->set(IntelCameraParameters::KEY_ISO, PlatformData::defaultIso(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_ISO, PlatformData::supportedIso(cameraId));

    // contrast control (Intel extension)
    intel_params->set(IntelCameraParameters::KEY_CONTRAST_MODE, PlatformData::defaultContrast(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_CONTRAST_MODES, PlatformData::supportedContrast(cameraId));

    // saturation control (Intel extension)
    intel_params->set(IntelCameraParameters::KEY_SATURATION_MODE, PlatformData::defaultSaturation(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_SATURATION_MODES, PlatformData::supportedSaturation(cameraId));

    // sharpness control (Intel extension)
    intel_params->set(IntelCameraParameters::KEY_SHARPNESS_MODE, PlatformData::defaultSharpness(cameraId));
    intel_params->set(IntelCameraParameters::KEY_SUPPORTED_SHARPNESS_MODES, PlatformData::supportedSharpness(cameraId));
}

const char* AtomISP::getMaxSnapShotResolution()
{
    LOG1("@%s", __FUNCTION__);
    int index = RESOLUTION_14MP;

    if (mConfig.snapshot.maxWidth < RESOLUTION_14MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_14MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_13MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_13MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_8MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_8MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_5MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_5MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_3MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_3MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_1080P_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_1080P_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_2MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_2MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_1_3MP_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_1_3MP_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_720P_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_720P_HEIGHT)
            index--;
    if (mConfig.snapshot.maxWidth < RESOLUTION_VGA_WIDTH || mConfig.snapshot.maxHeight < RESOLUTION_VGA_HEIGHT)
            index--;
    if (index < 0)
            index = 0;

    return resolution_tables[index];
}

/**
 * Applies ISP capture mode parameters to hardware
 *
 * Set latest requested values for capture mode parameters, and
 * pass them to kernel. These parameters cannot be set during
 * processing and are set only when starting capture.
 */
status_t AtomISP::updateCaptureParams()
{
    status_t status = NO_ERROR;
    if (mSensorType == SENSOR_TYPE_RAW) {
        if (atomisp_set_attribute(main_fd, V4L2_CID_ATOMISP_LOW_LIGHT, mLowLight, "Low Light") < 0) {
            LOGE("set low light failure");
            status = UNKNOWN_ERROR;
        }

        if (xioctl(main_fd, ATOMISP_IOC_S_XNR, &mXnr) < 0) {
            LOGE("set XNR failure");
            status = UNKNOWN_ERROR;
        }

        LOG2("capture params: xnr %d, anr %d", mXnr, mLowLight);
    }

    return status;
}

status_t AtomISP::getDvsStatistics(struct atomisp_dis_statistics *stats,
                                   bool *tryAgain) const
{
    /* This is a blocking call, so we do not lock a mutex here. The method
       is const, so the mutex is not needed anyway. */
    status_t status = NO_ERROR;
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_G_DIS_STAT, stats);
    if (tryAgain)
        *tryAgain = (errno == EAGAIN);
    if (errno == EAGAIN)
        return NO_ERROR;

    if (ret < 0) {
        LOGE("failed to get DVS statistics");
        status = UNKNOWN_ERROR;
    }
    return status;
}

status_t AtomISP::setMotionVector(const struct atomisp_dis_vector *vector) const
{
    status_t status = NO_ERROR;
    if (xioctl(main_fd, ATOMISP_IOC_S_DIS_VECTOR, (struct atomisp_dis_vector *)vector) < 0) {
        LOGE("failed to set motion vector");
        status = UNKNOWN_ERROR;
    }
    return status;
}

status_t AtomISP::setDvsCoefficients(const struct atomisp_dis_coefficients *coefs) const
{
    status_t status = NO_ERROR;
    if (xioctl(main_fd, ATOMISP_IOC_S_DIS_COEFS, (struct atomisp_dis_coefficients *)coefs) < 0) {
        LOGE("failed to set dvs coefficients");
        status = UNKNOWN_ERROR;
    }
    return status;
}

status_t AtomISP::getIspParameters(struct atomisp_parm *isp_param) const
{
    status_t status = NO_ERROR;
    if (xioctl(main_fd, ATOMISP_IOC_G_ISP_PARM, isp_param) < 0) {
        LOGE("failed to get ISP parameters");
        status = UNKNOWN_ERROR;
    }
    return status;
}

status_t AtomISP::applySensorFlip(void)
{
    int sensorFlip = PlatformData::sensorFlipping(mCameraInput->androidCameraId);

    if (sensorFlip == PlatformData::SENSOR_FLIP_NA)
        return NO_ERROR;

    if (atomisp_set_attribute(main_fd, V4L2_CID_VFLIP,
        (sensorFlip & PlatformData::SENSOR_FLIP_V)?1:0, "vertical image flip"))
        return UNKNOWN_ERROR;

    if (atomisp_set_attribute(main_fd, V4L2_CID_HFLIP,
        (sensorFlip & PlatformData::SENSOR_FLIP_H)?1:0, "horizontal image flip"))
        return UNKNOWN_ERROR;

    return NO_ERROR;
}

status_t AtomISP::configure(AtomMode mode)
{
    LOG1("@%s", __FUNCTION__);
    LOG1("mode = %d", mode);
    status_t status = NO_ERROR;

    switch (mode) {
    case MODE_PREVIEW:
        status = configurePreview();
        break;
    case MODE_VIDEO:
        status = configureRecording();
        break;
    case MODE_CAPTURE:
        status = configureCapture();
        break;
    case MODE_CONTINUOUS_CAPTURE:
        status = configureContinuous();
        break;
    default:
        status = UNKNOWN_ERROR;
        break;
    }

    if (status == NO_ERROR)
        mMode = mode;
    return status;
}

status_t AtomISP::allocateBuffers(AtomMode mode)
{
    LOG1("@%s", __FUNCTION__);
    LOG1("mode = %d", mode);
    status_t status = NO_ERROR;

    switch (mode) {
    case MODE_PREVIEW:
        mPreviewDevice = mConfigSnapshotPreviewDevice;
        if ((status = allocatePreviewBuffers()) != NO_ERROR)
            stopDevice(mPreviewDevice);
        if (mFileInject.active == true)
            startFileInject();
        break;
    case MODE_VIDEO:
        if ((status = allocateRecordingBuffers()) != NO_ERROR)
            return status;
        if (mFileInject.active == true)
            startFileInject();
        if ((status = allocatePreviewBuffers()) != NO_ERROR)
            stopRecording();
        if (mStoreMetaDataInBuffers) {
          if ((status = allocateMetaDataBuffers()) != NO_ERROR)
              stopRecording();
        }
        break;
    case MODE_CAPTURE:
        if ((status = allocateSnapshotBuffers()) != NO_ERROR)
            return status;
        if (mFileInject.active == true)
            startFileInject();
        break;
    case MODE_CONTINUOUS_CAPTURE:
        status = allocateBuffers(MODE_PREVIEW);
        if (status == NO_ERROR)
            status = allocateBuffers(MODE_CAPTURE);
        else
            freePreviewBuffers();
        break;

    default:
        status = UNKNOWN_ERROR;
        break;
    }

    return status;
}

status_t AtomISP::start()
{
    LOG1("@%s", __FUNCTION__);
    LOG1("mode = %d", mMode);
    status_t status = NO_ERROR;

    switch (mMode) {
    case MODE_PREVIEW:
        status = startPreview();
        break;
    case MODE_VIDEO:
        status = startRecording();
        break;
    case MODE_CAPTURE:
        status = startCapture();
        break;
    case MODE_CONTINUOUS_CAPTURE:
        status = startContinuousPreview();
        break;
    default:
        status = UNKNOWN_ERROR;
        break;
    };

    if (status == NO_ERROR) {
        runStartISPActions();
        mSessionId++;
    } else {
        mMode = MODE_NONE;
    }

    return status;
}

/**
 * Perform actions after ISP kernel device has
 * been started.
 */
void AtomISP::runStartISPActions()
{
    LOG1("@%s", __FUNCTION__);
    if (mFlashTorchSetting > 0) {
        setTorchHelper(mFlashTorchSetting);
    }

    // Start All ObserverThread's
    mObserverManager.setState(OBSERVER_STATE_RUNNING, NULL);
}

/**
 * Perform actions before ISP kernel device is closed.
 */
void AtomISP::runStopISPActions()
{
    LOG1("@%s", __FUNCTION__);
    if (mFlashTorchSetting > 0) {
        setTorchHelper(0);
    }
}

status_t AtomISP::stop()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    runStopISPActions();

    switch (mMode) {
    case MODE_PREVIEW:
        status = stopPreview();
        break;

    case MODE_VIDEO:
        status = stopRecording();
        break;

    case MODE_CAPTURE:
        status = stopCapture();
        break;

    case MODE_CONTINUOUS_CAPTURE:
        status = stopContinuousPreview();
        break;

    default:
        break;
    };

    if (status == NO_ERROR)
        mMode = MODE_NONE;

    return status;
}

status_t AtomISP::configurePreview()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;

    mNumPreviewBuffers = NUM_PREVIEW_BUFFERS;
    mPreviewDevice = mConfigSnapshotPreviewDevice;

    if (mPreviewDevice != V4L2_MAIN_DEVICE) {
        ret = openDevice(mPreviewDevice);
        if (ret < 0) {
            LOGE("Open preview device failed!");
            status = UNKNOWN_ERROR;
            return status;
        }
    }

    ret = configureDevice(
            mPreviewDevice,
            CI_MODE_PREVIEW,
            &(mConfig.preview),
            false);
    if (ret < 0) {
        status = UNKNOWN_ERROR;
        goto err;
    }

    // need to resend the current zoom value
    atomisp_set_zoom(main_fd, mConfig.zoom);

    return status;

err:
    stopDevice(mPreviewDevice);
    return status;
}

status_t AtomISP::startPreview()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;
    int i, initialSkips;

    ret = startDevice(mPreviewDevice, mNumPreviewBuffers);
    if (ret < 0) {
        LOGE("Start preview device failed!");
        status = UNKNOWN_ERROR;
        goto err;
    }

    /**
     * Some sensors produce corrupted first frames
     * If this sensor needs it then we skip
     */
    initialSkips = getNumOfSkipFrames();
    for (i = 0; i < initialSkips; i++) {
        AtomBuffer p;
        ret = getPreviewFrame(&p);
        if (ret == NO_ERROR) {
            ret = putPreviewFrame(&p);
            if (ret != NO_ERROR) {
                LOGE("Failed queueing preview frame!");
            }
        }
    }

    mNumPreviewBuffersQueued = mNumPreviewBuffers;

    PERFORMANCE_TRACES_BREAKDOWN_STEP_PARAM("Skip--", initialSkips);
    return status;

err:
    stopPreview();
    return status;
}

status_t AtomISP::stopPreview()
{
    LOG1("@%s", __FUNCTION__);

    status_t status = NO_ERROR;

    stopDevice(mPreviewDevice);
    freePreviewBuffers();

    if (mPreviewDevice != V4L2_MAIN_DEVICE)
        closeDevice(mPreviewDevice);

    if (mFileInject.active == true)
        stopFileInject();

    PERFORMANCE_TRACES_BREAKDOWN_STEP("Done");
    return status;
}

status_t AtomISP::configureRecording()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;
    FrameInfo *previewConfig;
    FrameInfo *recordingConfig;

    mPreviewDevice = mConfigRecordingPreviewDevice;

    ret = openDevice(mPreviewDevice);
    if (ret < 0) {
        LOGE("Open preview device failed!");
        status = UNKNOWN_ERROR;
        goto err;
    }

    // See function description of applyISPVideoLimitations(), Workaround 2
    if (mSwapRecordingDevice) {
        previewConfig = &(mConfig.recording);
        recordingConfig = &(mConfig.preview);
    } else {
        previewConfig  = &(mConfig.preview);
        recordingConfig = &(mConfig.recording);
    }

    ret = configureDevice(
            mRecordingDevice,
            CI_MODE_VIDEO,
            recordingConfig,
            false);
    if (ret < 0) {
        LOGE("Configure recording device failed!");
        status = UNKNOWN_ERROR;
        goto err;
    }

    mNumPreviewBuffers = PlatformData::getRecordingBufNum();
    ret = configureDevice(
            mPreviewDevice,
            CI_MODE_VIDEO,
            previewConfig,
            false);
    if (ret < 0) {
        LOGE("Configure recording device failed!");
        status = UNKNOWN_ERROR;
        goto err;
    }

    // The recording device must be configured first, so swap the devices after configuration
    if (mSwapRecordingDevice) {
        LOG1("@%s: swapping preview and recording devices", __FUNCTION__);
        int tmp = mPreviewDevice;
        mPreviewDevice = mRecordingDevice;
        mRecordingDevice = tmp;
        mRecordingDeviceSwapped = true;
    }

    return status;

err:
    stopRecording();
    return status;
}

status_t AtomISP::startRecording()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    status_t status = NO_ERROR;
    int i, initialSkips;

    ret = startDevice(mRecordingDevice, mNumBuffers);
    if (ret < 0) {
        LOGE("Start recording device failed");
        status = UNKNOWN_ERROR;
        goto err;
    }

    ret = startDevice(mPreviewDevice, mNumPreviewBuffers);
    if (ret < 0) {
        LOGE("Start preview device failed!");
        status = UNKNOWN_ERROR;
        goto err;
    }

    mNumPreviewBuffersQueued = mNumPreviewBuffers;
    mNumRecordingBuffersQueued = mNumBuffers;

    /**
     * Some sensors produce corrupted first frames
     * If this sensor needs it then we skip
     * TODO: This is wrong place to do it, it should be done
     * in the real consumer loop, since here we block the
     * start stack until frames come out.
     */
    initialSkips = getNumOfSkipFrames();
    for (i = 0; i < initialSkips; i++) {
        AtomBuffer p;
        ret = getPreviewFrame(&p);
        if (ret == NO_ERROR) {
            ret = putPreviewFrame(&p);
            if (ret != NO_ERROR) {
                LOGE("Failed queueing preview frame!");
            }
            ret = getRecordingFrame(&p);
            if (ret == NO_ERROR) {
                ret = putRecordingFrame(&p);
                if (ret != NO_ERROR) {
                    LOGE("Failed queueing recording frame!");
                }
            }

        }
    }

    return status;

err:
    stopRecording();
    return status;
}

status_t AtomISP::stopRecording()
{
    LOG1("@%s", __FUNCTION__);

    if (mRecordingDeviceSwapped) {
        LOG1("@%s: swapping preview and recording devices back", __FUNCTION__);
        int tmp = mPreviewDevice;
        mPreviewDevice = mRecordingDevice;
        mRecordingDevice = tmp;
        mRecordingDeviceSwapped = false;
    }

    stopDevice(mRecordingDevice);
    freeRecordingBuffers();

    stopDevice(mPreviewDevice);
    freePreviewBuffers();
    closeDevice(mPreviewDevice);

    if (mFileInject.active == true)
        stopFileInject();

    return NO_ERROR;
}

status_t AtomISP::configureCapture()
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    status_t status = NO_ERROR;

    updateCaptureParams();

    ret = configureDevice(
            V4L2_MAIN_DEVICE,
            CI_MODE_STILL_CAPTURE,
            &(mConfig.snapshot),
            isDumpRawImageReady());
    if (ret < 0) {
        LOGE("configure first device failed!");
        status = UNKNOWN_ERROR;
        goto errorFreeBuf;
    }

    ret = openDevice(V4L2_POSTVIEW_DEVICE);
    if (ret < 0) {
        LOGE("Open second device failed!");
        status = UNKNOWN_ERROR;
        goto errorFreeBuf;
    }

    ret = configureDevice(
            V4L2_POSTVIEW_DEVICE,
            CI_MODE_STILL_CAPTURE,
            &(mConfig.postview),
            false);
    if (ret < 0) {
        LOGE("configure second device failed!");
        status = UNKNOWN_ERROR;
        goto errorCloseSecond;
    }

    // Subscribe to frame sync event if in bracketing mode
    if (mFrameSyncRequested > 0) {
        ret = openDevice(V4L2_ISP_SUBDEV);
        if (ret < 0) {
            LOGE("Failed to open V4L2_ISP_SUBDEV!");
            goto errorCloseSecond;
        }

        ret = v4l2_subscribe_event(video_fds[V4L2_ISP_SUBDEV], V4L2_EVENT_FRAME_SYNC);
        if (ret < 0) {
            LOGE("Failed to subscribe to frame sync event!");
            goto errorCloseSubdev;
        }
        mFrameSyncEnabled = true;
    }

    // need to resend the current zoom value
    atomisp_set_zoom(main_fd, mConfig.zoom);

    return status;

errorCloseSubdev:
    closeDevice(V4L2_ISP_SUBDEV);
errorCloseSecond:
    closeDevice(V4L2_POSTVIEW_DEVICE);
errorFreeBuf:
    freeSnapshotBuffers();
    if (mFileInject.active == true)
        stopFileInject();

    return status;
}

/**
 * Configures continuos capture settings to kernel
 * (IOC_S_CONT_CAPTURE_CONFIG atomisp ioctl).
 *
 * This call has different semantics depending on whether
 * it's called before ISP is started, or when ISP is
 * already running. In the former case, this call is
 * used to configure the ringbuffer size. In the latter
 * case, this call is used to request ISP to start
 * rendering output (main and postview) frames with
 * the given parameters.
 *
 * See PSI BZ 80777 for a proposed change to this API.
 */
status_t AtomISP::requestContCapture(int numCaptures, int offset, unsigned int skip)
{
    LOG2("@%s", __FUNCTION__);

    struct atomisp_cont_capture_conf conf;

    CLEAR(conf);
    conf.num_captures = numCaptures;
    conf.offset = offset;
    conf.skip_frames = skip;

    int res = xioctl(video_fds[V4L2_MAIN_DEVICE],
                     ATOMISP_IOC_S_CONT_CAPTURE_CONFIG,
                     &conf);
    LOG1("@%s: CONT_CAPTURE_CONFIG num %d, offset %d, skip %u, res %d",
         __FUNCTION__, numCaptures, offset, skip, res);
    if (res != 0) {
        LOGE("@%s: error with CONT_CAPTURE_CONFIG, res %d", __FUNCTION__, res);
        return UNKNOWN_ERROR;
    }

    return NO_ERROR;
}

/**
 * Configures the ISP ringbuffer size in continuous mode.
 *
 * Set all ISP parameters that affect RAW ringbuffer
 * sizing in continuous mode.
 *
 * \see setContCaptureOffset(), setContCaptureSkip() and
 *      setContCaptureNumCaptures()
 */
status_t AtomISP::configureContinuousRingBuffer()
{
    int numBuffers = abs(mContCaptConfig.offset);
;
    int captures = mContCaptConfig.numCaptures;
    int offset = mContCaptConfig.offset;
    status_t status;

    if (captures > numBuffers)
        numBuffers = captures;

    if (numBuffers > PlatformData::maxContinuousRawRingBufferSize())
        numBuffers = PlatformData::maxContinuousRawRingBufferSize();

    LOG1("continuous mode ringbuffer size to %d (captures %d, offset %d)",
         numBuffers, captures, offset);

    status = requestContCapture(numBuffers,
                                mContCaptConfig.offset,
                                mContCaptConfig.skip);

    return status;
}

/**
 * Calculates the correct frame offset to capture to reach Zero
 * Shutter Lag.
 */
int AtomISP::shutterLagZeroAlign() const
{
    int delayMs = PlatformData::shutterLagCompensationMs();
    float frameIntervalMs = 1000.0 / getFrameRate();
    int lagZeroOffset = delayMs / frameIntervalMs + 1;
    LOG2("@%s: delay %dms, fps %.02f, zero offset %d",
         __FUNCTION__, delayMs, getFrameRate(), lagZeroOffset);
    return lagZeroOffset;
}

/**
 * Returns the minimum offset ISP supports.
 *
 * This values is the smallest value that can be passed
 * to prepareOfflineCapture() and startOfflineCapture().
 */
int AtomISP::continuousBurstNegMinOffset(void) const
{
    return -(PlatformData::maxContinuousRawRingBufferSize() - 2);
}

/**
 * Returns the needed buffer offset to capture frame
 * with negative time index 'startIndex' and when skippng
 * 'skip' input frames between each output frame.
 *
 * The resulting offset is aligned so that offset for
 * startIndex==0 matches the user perceived zero shutter lag
 * frame. This calibration is done by factoring in
 * PlatformData::shutterLagCompensationMs().
 *
 * As the ISP continuous capture buffer consists of frames stored
 * at full sensor rate, it depends on the requested output capture
 * rate, how much back in time one can go.
 *
 * \param skip how many input frames ISP skips after each output frame
 * \param startIndex index to first frame (counted at output rate)
 * \return negative offset to the requested frame (counted at full sensor rate)
 */
int AtomISP::continuousBurstNegOffset(int skip, int startIndex) const
{
    assert(startIndex <= 0);
    assert(skip >= 0);
    int targetRatio = skip + 1;
    int negOffset = targetRatio * startIndex - shutterLagZeroAlign();
    LOG2("@%s: offset %d, ratio %d, skip %d, align %d",
         __FUNCTION__, negOffset, targetRatio, skip, shutterLagZeroAlign());
    return negOffset;
}

status_t AtomISP::configureContinuous()
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    status_t status = NO_ERROR;

    if (!mContCaptPrepared) {
        LOGE("offline capture not prepared correctly");
        return UNKNOWN_ERROR;
    }

    updateCaptureParams();
    ret = configureContinuousRingBuffer();
    if (ret < 0) {
        LOGE("setting continuous capture params failed");
        return UNKNOWN_ERROR;
    }

    ret = configureDevice(
            V4L2_MAIN_DEVICE,
            CI_MODE_PREVIEW,
            &(mConfig.snapshot),
            isDumpRawImageReady());
    if (ret < 0) {
        LOGE("configure first device failed!");
        status = UNKNOWN_ERROR;
        goto errorFreeBuf;
    }

    status = configurePreview();
    if (status != NO_ERROR) {
        return status;
    }

    ret = openDevice(V4L2_POSTVIEW_DEVICE);
    if (ret < 0) {
        LOGE("Open second device failed!");
        status = UNKNOWN_ERROR;
        goto errorFreeBuf;
    }

    ret = configureDevice(
            V4L2_POSTVIEW_DEVICE,
            CI_MODE_PREVIEW,
            &(mConfig.postview),
            false);
    if (ret < 0) {
        LOGE("configure second device failed!");
        status = UNKNOWN_ERROR;
        goto errorCloseSecond;
    }

    // need to resend the current zoom value
    atomisp_set_zoom(main_fd, mConfig.zoom);

    return status;

errorCloseSecond:
    closeDevice(V4L2_POSTVIEW_DEVICE);
errorFreeBuf:
    freeSnapshotBuffers();
    if (mFileInject.active == true)
        stopFileInject();

    return status;
}

status_t AtomISP::startCapture()
{
    int ret;
    status_t status = NO_ERROR;
    int i, initialSkips;
    // Limited by driver, raw bayer image dump can support only 1 frame when setting
    // snapshot number. Otherwise, the raw dump image would be corrupted.
    int snapNum;
    if (CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_RAW))
        snapNum = 1;
    else
        snapNum = mConfig.num_snapshot;

    ret = startDevice(V4L2_MAIN_DEVICE, snapNum);
    if (ret < 0) {
        LOGE("start capture on first device failed!");
        status = UNKNOWN_ERROR;
        goto end;
    }

    ret = startDevice(V4L2_POSTVIEW_DEVICE, snapNum);
    if (ret < 0) {
        LOGE("start capture on second device failed!");
        status = UNKNOWN_ERROR;
        goto errorStopFirst;
    }

    /**
     * Some sensors produce corrupted first frames
     * If this sensor needs it then we skip
     */
    if (mMode != MODE_CONTINUOUS_CAPTURE)
        initialSkips = getNumOfSkipFrames();
    else
        initialSkips = 0;
    for (i = 0; i < initialSkips; i++) {
        AtomBuffer s,p;
        if (mFrameSyncEnabled)
            pollFrameSyncEvent();
        ret = getSnapshot(&s,&p);
        if (ret == NO_ERROR)
            ret = putSnapshot(&s,&p);
    }

    mNumCapturegBuffersQueued = snapNum;
    PERFORMANCE_TRACES_BREAKDOWN_STEP_PARAM("Skip--", initialSkips);
    return status;

errorStopFirst:
    stopDevice(V4L2_MAIN_DEVICE);
errorCloseSecond:
    closeDevice(V4L2_POSTVIEW_DEVICE);
errorFreeBuf:
    freeSnapshotBuffers();
    if (mFileInject.active == true)
        stopFileInject();

end:
    return status;
}

status_t AtomISP::stopContinuousPreview()
{
    LOG1("@%s", __FUNCTION__);
    int error = 0;
    if (stopCapture() != NO_ERROR)
        ++error;
    if (requestContCapture(0, 0, 0) != NO_ERROR)
        ++error;
    if (stopPreview() != NO_ERROR)
        ++error;
    if (error) {
        LOGE("@%s: errors (%d) in stopping continuous capture",
             __FUNCTION__, error);
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

/**
 * Checks whether local preview buffer pool contains shared buffers
 *
 * @param reserved optional argument to check if any of the shared
 *                 buffers are currently queued
 */
bool AtomISP::isSharedPreviewBufferConfigured(bool *reserved) const
{
    bool configured = false;

    if (reserved)
        *reserved = false;

    if (mPreviewBuffers)
        for (int i = 0 ; i < mNumPreviewBuffers; i++)
            if (mPreviewBuffers[i].shared) {
                configured = true;
                if (reserved && mPreviewBuffers[i].id == -1)
                    *reserved = true;
            }

    return configured;
}

status_t AtomISP::stopCapture()
{
    LOG1("@%s", __FUNCTION__);
    if (mDevices[V4L2_POSTVIEW_DEVICE].state == DEVICE_STARTED)
        stopDevice(V4L2_POSTVIEW_DEVICE);
    if (mDevices[V4L2_MAIN_DEVICE].state == DEVICE_STARTED)
        stopDevice(V4L2_MAIN_DEVICE);
    // note: MAIN device is kept open on purpose
    closeDevice(V4L2_POSTVIEW_DEVICE);
    // if SOF event is enabled, unsubscribe and close the device
    if (mFrameSyncEnabled) {
        v4l2_unsubscribe_event(video_fds[V4L2_ISP_SUBDEV], V4L2_EVENT_FRAME_SYNC);
        closeDevice(V4L2_ISP_SUBDEV);
        mFrameSyncEnabled = false;
    }
    if (mFileInject.active == true)
        stopFileInject();
    mUsingClientSnapshotBuffers = false;
    dumpRawImageFlush();
    PERFORMANCE_TRACES_BREAKDOWN_STEP("Done");
    return NO_ERROR;
}

status_t AtomISP::releaseCaptureBuffers()
{
    LOG1("@%s", __FUNCTION__);
    return freeSnapshotBuffers();
}

/**
 * Starts ISP in CSS1.5/2.0 continuous viewfinder mode.
 *
 * Queues buffers for all capture-related devices, including
 * preview, snapshot and postview devices. Then the preview
 * device is started with a STREAM_ON command, allowing the client
 * to start streaming preview data with getPreview() calls.
 *
 * To grab a picture without stopping preview, client should
 * call startOfflineCapture().
 */
status_t AtomISP::startContinuousPreview()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    status = prepareDevice(V4L2_MAIN_DEVICE, mConfig.num_snapshot);
    if (status != NO_ERROR)
        goto error;
    status = prepareDevice(V4L2_POSTVIEW_DEVICE, mConfig.num_snapshot);
    if (status != NO_ERROR)
        goto errorStopMain;
    status = startPreview();
    if (status != NO_ERROR)
        goto errorStopPostview;

    return status;

errorStopPostview:
    stopDevice(V4L2_POSTVIEW_DEVICE);
errorStopMain:
    stopDevice(V4L2_MAIN_DEVICE);
error:
    return status;
}

/**
 * Starts offline capture processing in the ISP.
 *
 * Snapshot and postview frame rendering is started
 * and frame(s) can be fetched with getSnapshot().
 *
 * Note that the capture params given in 'config' must be
 * equal or a subset of the configuration passed to
 * prepareOfflineCapture().
 *
 * \param config configuration container describing how many
 *               captures to take, skipping and the start offset
 */
status_t AtomISP::startOfflineCapture(AtomISP::ContinuousCaptureConfig &config)
{
    status_t res = NO_ERROR;

    LOG1("@%s", __FUNCTION__);
    if (mMode != MODE_CONTINUOUS_CAPTURE) {
        LOGE("@%s: invalid mode %d", __FUNCTION__, mMode);
        return INVALID_OPERATION;
    }
    else if (config.offset < mContCaptConfig.offset ||
             config.numCaptures > mContCaptConfig.numCaptures) {
        LOGE("@%s: cannot start with current ISP configuration", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    res = requestContCapture(config.numCaptures,
                             config.offset,
                             config.skip);
    if (res == NO_ERROR)
        res = startCapture();

    return res;
}

/**
 * Stops offline capture processing in the ISP:
 *
 * Performs a STREAM-OFF for snapshot and postview devices,
 * but does not free any buffers yet.
 */
status_t AtomISP::stopOfflineCapture()
{
    LOG1("@%s", __FUNCTION__);
    if (mMode != MODE_CONTINUOUS_CAPTURE) {
        LOGE("@%s: invalid mode %d", __FUNCTION__, mMode);
        return INVALID_OPERATION;
    }
    stopDevice(V4L2_MAIN_DEVICE, true);
    stopDevice(V4L2_POSTVIEW_DEVICE, true);
    mContCaptPrepared = true;
    return NO_ERROR;
}

/**
 * Prepares ISP for offline capture
 *
 * \param config container to configure capture count, skipping
 *               and the start offset (see struct AtomISP::ContinuousCaptureConfig)
 */
status_t AtomISP::prepareOfflineCapture(AtomISP::ContinuousCaptureConfig &cfg)
{
    LOG1("@%s, numCaptures = %d", __FUNCTION__, cfg.numCaptures);
    if (cfg.offset < continuousBurstNegMinOffset()) {
        LOGE("@%s: offset %d not supported, minimum %d",
             __FUNCTION__, cfg.offset, continuousBurstNegMinOffset());
        return UNKNOWN_ERROR;
    }
    mContCaptConfig = cfg;
    mContCaptPrepared = true;
    return NO_ERROR;
}

bool AtomISP::isOfflineCaptureRunning() const
{
    int device = V4L2_MAIN_DEVICE;
    VALID_DEVICE(device, false);

    if (mMode == MODE_CONTINUOUS_CAPTURE &&
        mDevices[device].state == DEVICE_STARTED)
        return true;

    return false;
}

bool AtomISP::isOfflineCaptureSupported() const
{
    // TODO: device node count reveals version of CSS firmware
    if (mConfigLastDevice >= 3)
        return true;

    return false;
}

bool AtomISP::isYUVvideoZoomingSupported() const
{
    // TODO: device node count reveals version of CSS firmware
    if (mConfigLastDevice >= 3)
        return true;

    return false;
}

/**
 * Configures a particular device with a mode (preview, video or capture)
 *
 * The FrameInfo struct contains information about the frame dimensions that
 * we are requesting to ISP
 * the field stride of the FrameInfo struct will be updated with the actual
 * width that the buffers need to have to meet the ISP constrains.
 * In effect the FrameInfo struct is an IN/OUT parameter.
 */
int AtomISP::configureDevice(int device, int deviceMode, FrameInfo *fInfo, bool raw)
{
    LOG1("@%s", __FUNCTION__);
    VALID_DEVICE(device, -1);
    int ret = 0;
    int w,h,format;
    w = fInfo->width;
    h = fInfo->height;
    format = fInfo->format;
    LOG1("device: %d, width:%d, height:%d, deviceMode:%d format:%d raw:%d", device,
        w, h, deviceMode, format, raw);

    if ((w <= 0) || (h <= 0)) {
        LOGE("Wrong Width %d or Height %d", w, h);
        return -1;
    }

    //Only update the configure for device
    int fd = video_fds[device];

    //Switch the Mode before set the format. This is the requirement of
    //atomisp
    ret = atomisp_set_capture_mode(deviceMode);
    if (ret < 0)
        return ret;

    if (device == V4L2_MAIN_DEVICE ||
        device == V4L2_PREVIEW_DEVICE)
        applySensorFlip();

    //Set the format
    ret = v4l2_capture_s_format(fd, device, w, h, format, raw, &(fInfo->stride));
    if (ret < 0)
        return ret;
    // update the size according to the stride from ISP
    fInfo->size = frameSize(fInfo->format, fInfo->stride, fInfo->height);
    v4l2_buf_pool[device].width = w;
    v4l2_buf_pool[device].height = h;
    v4l2_buf_pool[device].format = format;

    /* 3A related initialization*/
    //Reallocate the grid for 3A after format change
    if (device == V4L2_MAIN_DEVICE ||
        device == V4L2_PREVIEW_DEVICE) {
        ret = v4l2_capture_g_framerate(fd, &mConfig.fps, w, h, format);
        if (ret < 0) {
        /*Error handler: if driver does not support FPS achieving,
                       just give the default value.*/
            mConfig.fps = DEFAULT_SENSOR_FPS;
            ret = 0;
        }
    }

    // reduce FPS for still capture
    if (mFileInject.active == true) {
        if (deviceMode == CI_MODE_STILL_CAPTURE)
            mConfig.fps = 15;
    }

    mDevices[device].state = DEVICE_CONFIGURED;

    PERFORMANCE_TRACES_BREAKDOWN_STEP_PARAM("DeviceId:", device);
    //We need apply all the parameter settings when do the camera reset
    return ret;
}

int AtomISP::prepareDevice(int device, int buffer_count)
{
    LOG1("@%s, device = %d", __FUNCTION__, device);
    VALID_DEVICE(device, -1);

    int ret;
    int fd = video_fds[device];
    LOG1(" prepareDevice fd = %d", fd);

    //request, query and mmap the buffer and save to the pool
    ret = createBufferPool(device, buffer_count);
    if (ret < 0)
        return ret;

    mDevices[device].state = DEVICE_PREPARED;

    return 0;
}

int AtomISP::startDevice(int device, int buffer_count)
{
    LOG1("@%s, device = %d", __FUNCTION__, device);
    VALID_DEVICE(device, -1);

    int ret;
    int fd = video_fds[device];
    LOG1(" startDevice fd = %d", fd);

    if (mDevices[device].state != DEVICE_PREPARED) {
        ret = prepareDevice(device, buffer_count);
        if (ret < 0) {
            destroyBufferPool(device);
            return ret;
        }
    }

    //Qbuf
    ret = activateBufferPool(device);
    if (ret < 0)
        return ret;

    //stream on
    ret = v4l2_capture_streamon(fd);
    if (ret < 0)
        return ret;

    mDevices[device].frameCounter = 0;
    mDevices[device].state = DEVICE_STARTED;

    PERFORMANCE_TRACES_BREAKDOWN_STEP_PARAM("DeviceId:", device);
    return ret;
}

int AtomISP::activateBufferPool(int device)
{
    LOG1("@%s: device = %d", __FUNCTION__, device);

    int fd = video_fds[device];
    int ret;
    struct v4l2_buffer_pool *pool = &v4l2_buf_pool[device];

    for (int i = 0; i < pool->active_buffers; i++) {
        ret = v4l2_capture_qbuf(fd, i, &pool->bufs[i]);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int AtomISP::createBufferPool(int device, int buffer_count)
{
    LOG1("@%s: device = %d", __FUNCTION__, device);
    int i, ret;

    struct v4l2_buffer_pool *pool = &v4l2_buf_pool[device];
    int num_buffers = v4l2_capture_request_buffers(device, buffer_count);
    LOG1("num_buffers = %d", num_buffers);

    if (num_buffers <= 0)
        return -1;

    pool->active_buffers = num_buffers;

    for (i = 0; i < num_buffers; i++) {
        pool->bufs[i].width = pool->width;
        pool->bufs[i].height = pool->height;
        pool->bufs[i].format = pool->format;
        ret = v4l2_capture_new_buffer(device, i, &pool->bufs[i]);
        if (ret < 0)
            goto error;
    }
    return 0;

error:
    for (int j = 0; j < i; j++)
        v4l2_capture_free_buffer(device, &pool->bufs[j]);
    pool->active_buffers = 0;
    return ret;
}

int AtomISP::stopDevice(int device, bool leaveConfigured)
{
    LOG1("@%s: device = %d", __FUNCTION__, device);
    VALID_DEVICE(device, -1);

    int fd = video_fds[device];

    if (fd >= 0 && mDevices[device].state == DEVICE_STARTED) {
        //stream off
        v4l2_capture_streamoff(fd);

        if (!leaveConfigured) {
            destroyBufferPool(device);
            mDevices[device].state = DEVICE_CONFIGURED;
        }
        else {
            mDevices[device].state = DEVICE_PREPARED;
        }
    }
    return NO_ERROR;
}

void AtomISP::destroyBufferPool(int device)
{
    LOG1("@%s: device = %d", __FUNCTION__, device);

    struct v4l2_buffer_pool *pool = &v4l2_buf_pool[device];

    for (int i = 0; i < pool->active_buffers; i++)
        v4l2_capture_free_buffer(device, &pool->bufs[i]);
    pool->active_buffers = 0;
    v4l2_capture_release_buffers(device);
}

int AtomISP::openDevice(int device)
{
    LOG1("@%s", __FUNCTION__);
    if (video_fds[device] > 0) {
        LOGW("MainDevice already opened!");
        return video_fds[device];
    }

    video_fds[device] = v4l2_capture_open(device);

    LOGW("Open device %d with fd %d", device, video_fds[device]);

    if (video_fds[device] < 0) {
        LOGE("V4L2: capture_open failed: %s", strerror(errno));
        return -1;
    }

    // Query and check the capabilities
    if (device != V4L2_ISP_SUBDEV) {
        struct v4l2_capability cap;
        if (v4l2_capture_querycap(device, &cap) < 0) {
            LOGE("V4L2: capture_querycap failed: %s", strerror(errno));
            v4l2_capture_close(video_fds[device]);
            video_fds[device] = -1;
            return -1;
        }
    }

    mDevices[device].state = DEVICE_OPEN;

    return video_fds[device];
}

void AtomISP::closeDevice(int device)
{
    LOG1("@%s", __FUNCTION__);

    if (video_fds[device] < 0) {
        LOG1("Device %d already closed. Do nothing.", device);
        return;
    }

    v4l2_capture_close(video_fds[device]);

    video_fds[device] = -1;
    mDevices[device].state = DEVICE_CLOSED;
}

/**
 * Waits for frame data to be available
 *
 * \param device V4L2 device id
 * \param timeout time to wait for data (in ms), timeout of -1
 *        means to wait indefinitely for data
 *
 * \return 0: timeout, -1: error happened, positive number: success
 */
int AtomISP::v4l2_poll(int device, int timeout)
{
    LOG2("@%s", __FUNCTION__);
    struct pollfd pfd;
    int ret;

    if (video_fds[device] < 0) {
        LOG1("Device %d already closed. Do nothing.", device);
        return -1;
    }

    pfd.fd = video_fds[device];
    pfd.events = POLLPRI | POLLIN | POLLERR;


    ret = poll(&pfd, 1, timeout);

    if (pfd.revents & POLLERR) {
        LOG1("%s received POLLERR", __FUNCTION__);
        return -1;
    }

    return ret;
}

status_t AtomISP::selectCameraSensor()
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    int device = V4L2_MAIN_DEVICE;

    //Choose the camera sensor
    LOG1("Selecting camera sensor: %s", mCameraInput->name);
    ret = v4l2_capture_s_input(video_fds[device], mCameraInput->index);
    if (ret < 0) {
        LOGE("V4L2: capture_s_input failed: %s", strerror(errno));
        v4l2_capture_close(video_fds[device]);
        video_fds[device] = -1;
        return UNKNOWN_ERROR;
    }
    PERFORMANCE_TRACES_BREAKDOWN_STEP("capture_s_input");
    return NO_ERROR;
}

status_t AtomISP::setPreviewFrameFormat(int width, int height, int format)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if(format == 0)
         format = mConfig.preview.format;
    if (width > mConfig.preview.maxWidth || width <= 0)
        width = mConfig.preview.maxWidth;
    if (height > mConfig.preview.maxHeight || height <= 0)
        height = mConfig.preview.maxHeight;
    mConfig.preview.width = width;
    mConfig.preview.height = height;
    mConfig.preview.format = format;
    mConfig.preview.stride = width;
    mConfig.preview.size = frameSize(format, mConfig.preview.stride, height);
    LOG1("width(%d), height(%d), pad_width(%d), size(%d), format(%x)",
        width, height, mConfig.preview.stride, mConfig.preview.size, format);
    return status;
}

void AtomISP::getPostviewFrameFormat(int &width, int &height, int &format) const
{
    LOG1("@%s", __FUNCTION__);
    width = mConfig.postview.width;
    height = mConfig.postview.height;
    format = mConfig.postview.format;
}

status_t AtomISP::setPostviewFrameFormat(int width, int height, int format)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    LOG1("width(%d), height(%d), format(%x)",
         width, height, format);
    if (width < 0 || height < 0) {
        LOGE("Invalid postview size requested!");
        return BAD_VALUE;
    }
    if (width == 0 || height == 0) {
        // No thumbnail requested, we should anyway use postview to dequeue frames from ISP
        width = RESOLUTION_POSTVIEW_WIDTH;
        height = RESOLUTION_POSTVIEW_HEIGHT;
    }
    mConfig.postview.width = width;
    mConfig.postview.height = height;
    mConfig.postview.format = format;
    mConfig.postview.stride = width;
    mConfig.postview.size = frameSize(format, width, height);
    if (mConfig.postview.size == 0)
        mConfig.postview.size = mConfig.postview.width * mConfig.postview.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d), size(%d), format(%x)",
            width, height, mConfig.postview.stride, mConfig.postview.size, format);
    return status;
}

status_t AtomISP::setSnapshotFrameFormat(int width, int height, int format)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (width > mConfig.snapshot.maxWidth || width <= 0)
        width = mConfig.snapshot.maxWidth;
    if (height > mConfig.snapshot.maxHeight || height <= 0)
        height = mConfig.snapshot.maxHeight;
    mConfig.snapshot.width  = width;
    mConfig.snapshot.height = height;
    mConfig.snapshot.format = format;
    mConfig.snapshot.stride = width;
    mConfig.snapshot.size = frameSize(format, width, height);;
    if (mConfig.snapshot.size == 0)
        mConfig.snapshot.size = mConfig.snapshot.width * mConfig.snapshot.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d), size(%d), format(%x)",
        width, height, mConfig.snapshot.stride, mConfig.snapshot.size, format);
    return status;
}

void AtomISP::getVideoSize(int *width, int *height, int *stride = NULL)
{
    if (width && height) {
        *width = mConfig.recording.width;
        *height = mConfig.recording.height;
    }
    if (stride)
        *stride = mConfig.recording.stride;
}

void AtomISP::getPreviewSize(int *width, int *height, int *stride = NULL)
{
    if (width && height) {
        *width = mConfig.preview.width;
        *height = mConfig.preview.height;
    }
    if (stride)
        *stride = mConfig.preview.stride;
}

int AtomISP::getSnapshotNum()
{
    return mConfig.num_snapshot;
}

status_t AtomISP::setSnapshotNum(int num)
{
    LOG1("@%s", __FUNCTION__);

    if (mMode != MODE_NONE)
        return INVALID_OPERATION;

    // 'num_snapshot' is used when freeing the buffers, so to keep track,
    // deallocate with old value here
    if (mConfig.num_snapshot != num)
        freeSnapshotBuffers();

    mConfig.num_snapshot = num;
    LOG1("mConfig.num_snapshot = %d", mConfig.num_snapshot);
    return NO_ERROR;
}

status_t AtomISP::setVideoFrameFormat(int width, int height, int format)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    /**
     * Workaround: When video size is 1080P(1920x1080), because video HW codec
     * requests 16x16 pixel block as sub-block to encode, So whatever apps set recording
     * size to 1080P, ISP always outputs 1920x1088
     * for encoder.
     * In current supported list of video size, only height 1080(1920x1080) isn't multiple of 16
     */
    if(height % 16)
        height = (height + 15) / 16 * 16;

    if(format == 0)
         format = mConfig.recording.format;
    if (mConfig.recording.width == width &&
        mConfig.recording.height == height &&
        mConfig.recording.format == format) {
        // Do nothing
        return status;
    }

    if (mMode == MODE_VIDEO) {
        LOGE("Reconfiguration in video mode unsupported. Stop the ISP first");
        return INVALID_OPERATION;
    }

    if (width > mConfig.recording.maxWidth || width <= 0) {
        LOGE("invalid recording width %d. override to %d", width, mConfig.recording.maxWidth);
        width = mConfig.recording.maxWidth;
    }
    if (height > mConfig.recording.maxHeight || height <= 0) {
        LOGE("invalid recording height %d. override to %d", height, mConfig.recording.maxHeight);
        height = mConfig.recording.maxHeight;
    }
    mConfig.recording.width = width;
    mConfig.recording.height = height;
    mConfig.recording.format = format;
    mConfig.recording.stride = width;
    mConfig.recording.size = frameSize(format, width, height);
    if (mConfig.recording.size == 0)
        mConfig.recording.size = mConfig.recording.width * mConfig.recording.height * BPP;
    LOG1("width(%d), height(%d), pad_width(%d), format(%x)",
            width, height, mConfig.recording.stride, format);

    return status;
}

/**
 * Apply ISP limitations related to supported preview sizes when in video mode.
 *
 * Workaround 1: with DVS enable, the fps in 1080p recording can't reach 30fps,
 * so check if the preview size is corresponding to recording, if yes, then
 * change preview size to 640x360
 * BZ: 49330 51853
 *
 * Workaround 2: The camera firmware doesn't support preview dimensions that
 * are bigger than video dimensions. If a single preview dimension is larger
 * than the video dimension then the preview and recording devices will be
 * swapped to work around this limitation.
 *
 * Workaround 3: With some sensors, the configuration for 1080p
 * recording does not give enough processing time (blanking time) to
 * the ISP, so the viewfinder resolution must be limited.
 * BZ: 55640 59636
 *
 * @param params
 * @param dvsEabled
 * @return true: updated preview size
 * @return false: not need to update preview size
 */
bool AtomISP::applyISPVideoLimitations(CameraParameters *params, bool dvsEnabled)
{
    LOG1("@%s", __FUNCTION__);
    bool ret = false;
    int previewWidth, previewHeight;
    int videoWidth, videoHeight;
    bool reducedVf = false;

    params->getPreviewSize(&previewWidth, &previewHeight);
    params->getVideoSize(&videoWidth, &videoHeight);

    // Workaround 3: with some sensors the VF resolution must be
    //               limited high-resolution video recordiing
    // TODO: if we get more cases like this, move to PlatformData.h
    const char* sensorName = "ov8830";
    if (mCameraInput &&
        strncmp(mCameraInput->name, sensorName, sizeof(sensorName) - 1) == 0) {
        LOG1("Quirk for sensor %s, limiting video preview size", mCameraInput->name);
        reducedVf = true;
    }

    // Workaround 1+3, detail refer to the function description
    if (reducedVf || dvsEnabled) {
        if ((previewWidth > RESOLUTION_VGA_WIDTH || previewHeight > RESOLUTION_VGA_HEIGHT) &&
            (videoWidth > RESOLUTION_720P_WIDTH || videoHeight > RESOLUTION_720P_HEIGHT)) {
                ret = true;
                params->setPreviewSize(640, 360);
                LOG1("change preview size to 640x360 due to DVS on");
            } else {
                LOG1("no need change preview size: %dx%d", previewWidth, previewHeight);
            }
    }
    //Workaround 2, detail refer to the function description
    params->getPreviewSize(&previewWidth, &previewHeight);
    params->getVideoSize(&videoWidth, &videoHeight);
    if((previewWidth*previewHeight) > (videoWidth*videoHeight)) {
            ret = true;
            mSwapRecordingDevice = true;
            LOG1("Video dimension(s) [%d, %d] is smaller than preview dimension(s) [%d, %d]. "
                 "Triggering swapping of preview and recording devices.",
                 videoWidth, videoHeight, previewWidth, previewHeight);
    } else {
        mSwapRecordingDevice = false;
    }

    return ret;
}

void AtomISP::getZoomRatios(bool videoMode, CameraParameters *params)
{
    LOG1("@%s", __FUNCTION__);
    if (params) {
        if (!isYUVvideoZoomingSupported() && videoMode && mSensorType == SENSOR_TYPE_SOC) {
            // zoom is not supported. this is indicated by placing a single zoom ratio in params
            params->set(CameraParameters::KEY_ZOOM, "0");
            params->set(CameraParameters::KEY_MAX_ZOOM, "0"); // zoom index 0 indicates first (and only) zoom ratio
            params->set(CameraParameters::KEY_ZOOM_RATIOS, "100");
            params->set(CameraParameters::KEY_ZOOM_SUPPORTED, CameraParameters::FALSE);
        } else {
            params->set(CameraParameters::KEY_MAX_ZOOM, MAX_ZOOM_LEVEL);
            params->set(CameraParameters::KEY_ZOOM_RATIOS, mZoomRatios);
            params->set(CameraParameters::KEY_ZOOM_SUPPORTED, CameraParameters::TRUE);
        }
    }
}

void AtomISP::getFocusDistances(CameraParameters *params)
{
    LOG1("@%s", __FUNCTION__);
    char focusDistance[100] = {0};
    float fDistances[3] = {0};  // 3 distances: near, optimal, and far

    // would be better if we could get these from driver instead of hard-coding
    if(mCameraInput->port == ATOMISP_CAMERA_PORT_PRIMARY) {
        fDistances[0] = 2.0;
        fDistances[1] = 2.0;
        fDistances[2] = INFINITY;
    }
    else {
        fDistances[0] = 0.3;
        fDistances[1] = 0.65;
        fDistances[2] = INFINITY;
    }

    for (int i = 0; i < (int) (sizeof(fDistances)/sizeof(fDistances[0])); i++) {
        int left = sizeof(focusDistance) - strlen(focusDistance);
        int res;

        // use CameraParameters::FOCUS_DISTANCE_INFINITY for value of infinity
        if (fDistances[i] == INFINITY) {
            res = snprintf(focusDistance + strlen(focusDistance), left, "%s%s",
                    i ? "," : "", CameraParameters::FOCUS_DISTANCE_INFINITY);
        } else {
            res = snprintf(focusDistance + strlen(focusDistance), left, "%s%g",
                    i ? "," : "", fDistances[i]);
        }
        if (res < 0) {
            LOGE("Could not generate %s string: %s",
                CameraParameters::KEY_FOCUS_DISTANCES, strerror(errno));
            return;
        }
    }
    params->set(CameraParameters::KEY_FOCUS_DISTANCES, focusDistance);
}

status_t AtomISP::setFlash(int numFrames)
{
    LOG1("@%s: numFrames = %d", __FUNCTION__, numFrames);
    if (mCameraInput->port != ATOMISP_CAMERA_PORT_PRIMARY) {
        LOGE("Flash is supported only for primary camera!");
        return INVALID_OPERATION;
    }
    if (numFrames) {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_FLASH, "Flash Mode flash") < 0)
            return UNKNOWN_ERROR;
        if (atomisp_set_attribute(main_fd, V4L2_CID_REQUEST_FLASH, numFrames, "Request Flash") < 0)
            return UNKNOWN_ERROR;
    } else {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_OFF, "Flash Mode flash") < 0)
            return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setFlashIndicator(int intensity)
{
    LOG1("@%s: intensity = %d", __FUNCTION__, intensity);
    if (mCameraInput->port != ATOMISP_CAMERA_PORT_PRIMARY) {
        LOGE("Indicator intensity is supported only for primary camera!");
        return INVALID_OPERATION;
    }

    if (intensity) {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_INDICATOR_INTENSITY, intensity, "Indicator Intensity") < 0)
            return UNKNOWN_ERROR;
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_INDICATOR, "Flash Mode") < 0)
            return UNKNOWN_ERROR;
    } else {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_OFF, "Flash Mode") < 0)
            return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setTorchHelper(int intensity)
{
    if (intensity) {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_TORCH_INTENSITY, intensity, "Torch Intensity") < 0)
            return UNKNOWN_ERROR;
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_TORCH, "Flash Mode") < 0)
            return UNKNOWN_ERROR;
    } else {
        if (atomisp_set_attribute(main_fd, V4L2_CID_FLASH_MODE, ATOMISP_FLASH_MODE_OFF, "Flash Mode") < 0)
            return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setTorch(int intensity)
{
    LOG1("@%s: intensity = %d", __FUNCTION__, intensity);

    if (mCameraInput->port != ATOMISP_CAMERA_PORT_PRIMARY) {
        LOGE("Indicator intensity is supported only for primary camera!");
        return INVALID_OPERATION;
    }

    setTorchHelper(intensity);

    // closing the kernel device will not automatically turn off
    // flash light, so need to keep track in user-space
    mFlashTorchSetting = intensity;

    return NO_ERROR;
}

status_t AtomISP::setColorEffect(v4l2_colorfx effect)
{
    mColorEffect = effect;
    return NO_ERROR;
}

status_t AtomISP::applyColorEffect()
{
    LOG2("@%s: effect = %d", __FUNCTION__, mColorEffect);
    status_t status = NO_ERROR;

    // Color effect overrides configs that AIC has set
    // Apply only when color effect is selected
    if (mColorEffect != 0){
        if (atomisp_set_attribute (main_fd, V4L2_CID_COLORFX, mColorEffect, "Colour Effect") < 0){
            LOGE("Error setting color effect");
            return UNKNOWN_ERROR;
        }
    }
    return status;
}

status_t AtomISP::setZoom(int zoom)
{
    LOG1("@%s: zoom = %d", __FUNCTION__, zoom);
    if (zoom == mConfig.zoom)
        return NO_ERROR;
    if (mMode == MODE_CAPTURE)
        return NO_ERROR;

    int ret = atomisp_set_zoom(main_fd, zoom);
    if (ret < 0) {
        LOGE("Error setting zoom to %d", zoom);
        return UNKNOWN_ERROR;
    }
    mConfig.zoom = zoom;
    return NO_ERROR;
}

status_t AtomISP::getMakerNote(atomisp_makernote_info *info)
{
    LOG1("@%s: info = %p", __FUNCTION__, info);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }
    info->focal_length = 0;
    info->f_number_curr = 0;
    info->f_number_range = 0;
    if (xioctl(fd, ATOMISP_IOC_ISP_MAKERNOTE, info) < 0) {
        LOGW("WARNING: get maker note from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::getContrast(int *value)
{
    LOG1("@%s", __FUNCTION__);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }

    LOG2("@%s", __FUNCTION__);
    if (atomisp_get_attribute(fd,V4L2_CID_CONTRAST, value) < 0) {
        LOGW("WARNING: get Contrast from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setContrast(int value)
{
    LOG1("@%s: value:%d", __FUNCTION__, value);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }
    if (atomisp_set_attribute(fd, V4L2_CID_CONTRAST, value, "Request Contrast") < 0) {
        LOGW("WARNING: set Contrast from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::getSaturation(int *value)
{
    LOG1("@%s", __FUNCTION__);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }

    LOG2("@%s", __FUNCTION__);
    if (atomisp_get_attribute(fd,V4L2_CID_SATURATION, value) < 0) {
        LOGW("WARNING: get Saturation from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setSaturation(int value)
{
    LOG1("@%s: value:%d", __FUNCTION__, value);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }
    if (atomisp_set_attribute(fd, V4L2_CID_SATURATION, value, "Request Saturation") < 0) {
        LOGW("WARNING: set Saturation from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::getSharpness(int *value)
{
    LOG1("@%s", __FUNCTION__);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }

    LOG2("@%s", __FUNCTION__);
    if (atomisp_get_attribute(fd,V4L2_CID_SHARPNESS, value) < 0) {
        LOGW("WARNING: get Sharpness from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setSharpness(int value)
{
    LOG1("@%s: value:%d", __FUNCTION__, value);
    int fd = video_fds[V4L2_MAIN_DEVICE];

    if (fd < 0) {
        return INVALID_OPERATION;
    }
    if (atomisp_set_attribute(fd, V4L2_CID_SHARPNESS, value, "Request Sharpness") < 0) {
        LOGW("WARNING: set Sharpness from driver failed!");
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::setXNR(bool enable)
{
    LOG1("@%s: %d", __FUNCTION__, (int) enable);
    mXnr = enable;
    return NO_ERROR;
}

status_t AtomISP::setDVS(bool enable)
{
    LOG1("@%s: %d", __FUNCTION__, enable);
    status_t status = NO_ERROR;
    status = atomisp_set_attribute(main_fd, V4L2_CID_ATOMISP_VIDEO_STABLIZATION,
                                    enable, "Video Stabilization");
    if(status != 0)
    {
        LOGE("Error setting DVS in the driver");
        status = INVALID_OPERATION;
    }

    return status;
}

status_t AtomISP::setGDC(bool enable)
{
    LOG1("@%s: %d", __FUNCTION__, enable);
    status_t status;
    status = atomisp_set_attribute(main_fd, V4L2_CID_ATOMISP_POSTPROCESS_GDC_CAC,
                                   enable, "GDC");

    return status;
}

status_t AtomISP::setAeFlickerMode(FlickerMode mode) {

    LOG1("@%s: %d", __FUNCTION__, (int) mode);
    status_t status(NO_ERROR);
    int ret(0);
    v4l2_power_line_frequency theMode(V4L2_CID_POWER_LINE_FREQUENCY_DISABLED);

    if (mSensorType != SENSOR_TYPE_RAW) {

        switch(mode) {
        case CAM_AE_FLICKER_MODE_50HZ:
            theMode = V4L2_CID_POWER_LINE_FREQUENCY_50HZ;
            break;
        case CAM_AE_FLICKER_MODE_60HZ:
            theMode = V4L2_CID_POWER_LINE_FREQUENCY_60HZ;
            break;
        case CAM_AE_FLICKER_MODE_OFF:
            theMode = V4L2_CID_POWER_LINE_FREQUENCY_DISABLED;
            break;
        case CAM_AE_FLICKER_MODE_AUTO:
            theMode = V4L2_CID_POWER_LINE_FREQUENCY_AUTO;
            break;
        default:
            LOGE("unsupported light frequency mode(%d)", (int) mode);
            status = BAD_VALUE;
            return status;
        }
        ret = atomisp_set_attribute(main_fd, V4L2_CID_POWER_LINE_FREQUENCY,
                                    theMode, "light frequency");
        if (ret < 0) {
            LOGE("setting light frequency failed");
            status = UNKNOWN_ERROR;
        }
    }
    return status;
}

status_t AtomISP::setLowLight(bool enable)
{
    LOG1("@%s: %d", __FUNCTION__, (int) enable);
    mLowLight = enable;
    return NO_ERROR;
}

int AtomISP::atomisp_set_zoom (int fd, int zoom)
{
    LOG1("@%s", __FUNCTION__);
    if (fd < 0) {
        LOG1("Device not opened!");
        return 0;
    }

    int zoom_driver = 0;
    float zoom_real = 0.0;

    if (zoom != 0) {

        /*
           The zoom value passed to HAL is from 0 to MAX_ZOOM_LEVEL to match 1x
           to 16x of real zoom effect. The equation between zoom_real and zoom_hal is:

           (zoom_hal - MIN_ZOOM_LEVEL)                   MAX_ZOOM_LEVEL - MIN_ZOOM_LEVEL
           ------------------------------------------ = ------------------------------------
           zoom_real * ZOOM_RATIO - MIN_SUPPORT_ZOOM     MAX_SUPPORT_ZOOM - MIN_SUPPORT_ZOOM
         */

        float x = ((MAX_SUPPORT_ZOOM - MIN_SUPPORT_ZOOM) / (MAX_ZOOM_LEVEL - MIN_ZOOM_LEVEL)) *
            ((float) zoom - MIN_ZOOM_LEVEL);
        zoom_real = (x + MIN_SUPPORT_ZOOM) / ZOOM_RATIO;

        /*
           The real zoom effect is 64/(64-zoom_driver) in the driver.
           Add 0.5 to get the more accurate result
           Calculate the zoom value should set to driver using the equation
           We want to get 3 if the zoom_driver is 2.9, so add 0.5 for compensation
         */
        int maxZoomFactor = PlatformData::getMaxZoomFactor();
        zoom_driver = (maxZoomFactor - (maxZoomFactor / zoom_real) + 0.5);
    }

    LOG1("set zoom %f to driver with %d", zoom_real, zoom_driver);
    return atomisp_set_attribute (fd, V4L2_CID_ZOOM_ABSOLUTE, zoom_driver, "zoom");
}

int AtomISP::atomisp_set_attribute (int fd, int attribute_num,
                                             const int value, const char *name)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_control control;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control ext_control;

    LOG1("setting attribute [%s] to %d", name, value);

    if (fd < 0)
        return -1;

    control.id = attribute_num;
    control.value = value;
    controls.ctrl_class = V4L2_CTRL_ID2CLASS(control.id);
    controls.count = 1;
    controls.controls = &ext_control;
    ext_control.id = attribute_num;
    ext_control.value = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) == 0)
        return 0;

    if (ioctl(fd, VIDIOC_S_CTRL, &control) == 0)
        return 0;

    LOGE("Failed to set value %d for control %s (%d) on device '%d', %s",
        value, name, attribute_num, fd, strerror(errno));
    return -1;
}

/**
 * atomisp_get_attribute():
 * Try to get the value of one specific attribute
 * return value: 0 for success
 *               others are errors
 */
int AtomISP::atomisp_get_attribute (int fd, int attribute_num,
                                    int *value)
{
    struct v4l2_control control;
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control ext_control;

    if (fd < 0)
        return -1;

    control.id = attribute_num;
    controls.ctrl_class = V4L2_CTRL_ID2CLASS(control.id);
    controls.count = 1;
    controls.controls = &ext_control;
    ext_control.id = attribute_num;

    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
        *value = ext_control.value;
    return 0;
    }

    if (ioctl(fd, VIDIOC_G_CTRL, &control) == 0) {
        *value = control.value;
        return 0;
    }

    LOGE("Failed to get value for control (%d) on device '%d', %s",
          attribute_num, fd, strerror(errno));
    return -1;
}

int AtomISP::xioctl(int fd, int request, void *arg) const
{
    int ret;

    do {
        ret = ioctl (fd, request, arg);
    } while (-1 == ret && EINTR == errno);

    if (ret < 0)
        LOGW ("%s: Request 0x%x failed: %s", __FUNCTION__, request, strerror(errno));

    return ret;
}

/**
 * Start inject image data from a file using the special-purpose
 * V4L2 device node.
 */
int AtomISP::startFileInject(void)
{
    LOG1("%s: enter", __FUNCTION__);

    int ret = 0;
    int device = V4L2_INJECT_DEVICE;
    int buffer_count = 1;

    if (mFileInject.active != true) {
        LOGE("%s: no input file set",  __func__);
        return -1;
    }

    video_fds[device] = v4l2_capture_open(device);

    if (video_fds[device] < 0)
        goto error1;

    // Query and check the capabilities
    struct v4l2_capability cap;
    if (v4l2_capture_querycap(device, &cap) < 0)
        goto error1;

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.outputmode = OUTPUT_MODE_FILE;
    if (ioctl(video_fds[device], VIDIOC_S_PARM, &parm) < 0) {
        LOGE("error %s", strerror(errno));
        return -1;
    }

    if (fileInjectSetSize() != NO_ERROR)
        goto error1;

    //Set the format
    ret = v4l2_capture_s_format(video_fds[device], device, mFileInject.width,
                                mFileInject.height, mFileInject.format, false, &(mFileInject.stride));
    if (ret < 0)
        goto error1;

    v4l2_buf_pool[device].width = mFileInject.width;
    v4l2_buf_pool[device].height = mFileInject.height;
    v4l2_buf_pool[device].format = mFileInject.format;

    //request, query and mmap the buffer and save to the pool
    ret = createBufferPool(device, buffer_count);
    if (ret < 0)
        goto error1;

    // QBUF
    ret = activateBufferPool(device);
    if (ret < 0)
        goto error0;

    return 0;

error0:
    destroyBufferPool(device);
error1:
    v4l2_capture_close(video_fds[device]);
    video_fds[device] = -1;
    return -1;
}

/**
 * Stops file injection.
 *
 * Closes the kernel resources needed for file injection and
 * other resources.
 */
int AtomISP::stopFileInject(void)
{
    LOG1("%s: enter", __FUNCTION__);
    int device;
    device = V4L2_INJECT_DEVICE;
    if (video_fds[device] < 0)
        LOGW("%s: Already closed", __func__);
    destroyBufferPool(device);
    v4l2_capture_close(video_fds[device]);
    video_fds[device] = -1;
    return 0;
}

/**
 * Configures image data injection.
 *
 * If 'fileName' is non-NULL, file injection is enabled with the given
 * settings. Once enabled, file injection will be performend when
 * start() is issued, and stopped when stop() is issued. Injection
 * applies to all device modes.
 */
int AtomISP::configureFileInject(const char *fileName, int width, int height, int format, int bayerOrder)
{
    LOG1("%s: enter", __FUNCTION__);
    mFileInject.fileName = String8(fileName);
    if (mFileInject.fileName.isEmpty() != true) {
        LOG1("Enabling file injection, image file=%s", mFileInject.fileName.string());
        mFileInject.active = true;
        mFileInject.width = width;
        mFileInject.height = height;
        mFileInject.format = format;
        mFileInject.bayerOrder = bayerOrder;
    }
    else {
        mFileInject.active = false;
        LOG1("Disabling file injection");
    }
    return 0;
}

status_t AtomISP::fileInjectSetSize(void)
{
    int fileFd = -1;
    int fileSize = 0;
    struct stat st;
    const char* fileName = mFileInject.fileName.string();

    /* Open the file we will transfer to kernel */
    if ((fileFd = open(mFileInject.fileName.string(), O_RDONLY)) == -1) {
        LOGE("ERR(%s): Failed to open %s\n", __FUNCTION__, fileName);
        return INVALID_OPERATION;
    }

    CLEAR(st);
    if (fstat(fileFd, &st) < 0) {
        LOGE("ERR(%s): fstat %s failed\n", __func__, fileName);
        return INVALID_OPERATION;
    }

    fileSize = st.st_size;
    if (fileSize == 0) {
        LOGE("ERR(%s): empty file %s\n", __func__, fileName);
        return -1;
    }

    LOG1("%s: file %s size of %u", __FUNCTION__, fileName, fileSize);

    mFileInject.size = fileSize;

    close(fileFd);
    return NO_ERROR;
}

int AtomISP::v4l2_capture_streamon(int fd)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        LOGE("VIDIOC_STREAMON returned: %d (%s)", ret, strerror(errno));
        return ret;
    }
    return ret;
}

int AtomISP::v4l2_capture_streamoff(int fd)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (fd < 0){ //Device is closed
        LOGE("Device is closed!");
        return 0;
    }
    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        LOGE("VIDIOC_STREAMOFF returned: %d (%s)", ret, strerror(errno));
        return ret;
    }

    return ret;
}

/* Unmap the buffer or free the userptr */
int AtomISP::v4l2_capture_free_buffer(int device, struct v4l2_buffer_info *buf_info)
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    void *addr = buf_info->data;
    size_t length = buf_info->length;

    if (device == V4L2_INJECT_DEVICE &&
        (ret = munmap(addr, length)) < 0) {
            LOGE("munmap returned: %d (%s)", ret, strerror(errno));
            return ret;
        }

    return ret;
}

int AtomISP::v4l2_capture_release_buffers(int device)
{
    LOG1("@%s", __FUNCTION__);
    return v4l2_capture_request_buffers(device, 0);
}

int AtomISP::v4l2_capture_request_buffers(int device, uint num_buffers)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_requestbuffers req_buf;
    int ret;
    CLEAR(req_buf);

    int fd = video_fds[device];

    if (fd < 0)
        return 0;

    req_buf.memory = V4L2_MEMORY_USERPTR;
    req_buf.count = num_buffers;
    req_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (device == V4L2_INJECT_DEVICE) {
        req_buf.memory = V4L2_MEMORY_MMAP;
        req_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    }

    LOG1("VIDIOC_REQBUFS, count=%d", req_buf.count);
    ret = ioctl(fd, VIDIOC_REQBUFS, &req_buf);

    if (ret < 0) {
        LOGE("VIDIOC_REQBUFS(%d) returned: %d (%s)",
            num_buffers, ret, strerror(errno));
        return ret;
    }

    if (req_buf.count < num_buffers)
        LOGW("Got less buffers than requested!");

    return req_buf.count;
}

int AtomISP::v4l2_capture_new_buffer(int device, int index, struct v4l2_buffer_info *buf)
{
    LOG1("@%s", __FUNCTION__);
    void *data;
    int ret;
    int fd = video_fds[device];
    struct v4l2_buffer *vbuf = &buf->vbuffer;
    vbuf->flags = 0x0;

    if (device == V4L2_INJECT_DEVICE) {
        vbuf->index = index;
        vbuf->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        vbuf->memory = V4L2_MEMORY_MMAP;

        ret = ioctl(fd, VIDIOC_QUERYBUF, vbuf);
        if (ret < 0) {
            LOGE("VIDIOC_QUERYBUF failed: %s", strerror(errno));
            return -1;
        }

        data = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    vbuf->m.offset);

        if (MAP_FAILED == data) {
            LOGE("mmap failed: %s", strerror(errno));
            return -1;
        }

        buf->data = data;
        buf->length = vbuf->length;

        // fill buffer with image data from file
        FILE *file;
        if (!(file = fopen(mFileInject.fileName.string(), "r"))) {
            LOGE("ERR(%s): Failed to open %s\n", __func__, mFileInject.fileName.string());
            return -1;
        }
        fread(data, 1,  mFileInject.size, file);
        fclose(file);
        return 0;
    }

    vbuf->memory = V4L2_MEMORY_USERPTR;

    vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf->index = index;
    ret = ioctl(fd , VIDIOC_QUERYBUF, vbuf);

    if (ret < 0) {
        LOGE("VIDIOC_QUERYBUF failed: %s", strerror(errno));
        return ret;
    }

    vbuf->m.userptr = (unsigned int)(buf->data);

    buf->length = vbuf->length;
    LOG1("index %u", vbuf->index);
    LOG1("type %d", vbuf->type);
    LOG1("bytesused %u", vbuf->bytesused);
    LOG1("flags %08x", vbuf->flags);
    LOG1("memory %u", vbuf->memory);
    LOG1("userptr:  %lu", vbuf->m.userptr);
    LOG1("length %u", vbuf->length);
    return ret;
}

int AtomISP::v4l2_capture_g_framerate(int fd, float *framerate, int width,
                                         int height, int pix_fmt)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_frmivalenum frm_interval;

    if (NULL == framerate)
        return -EINVAL;

    assert(fd > 0);
    CLEAR(frm_interval);
    frm_interval.pixel_format = pix_fmt;
    frm_interval.width = width;
    frm_interval.height = height;
    *framerate = -1.0;

    ret = ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frm_interval);
    if (ret < 0) {
        LOGW("ioctl failed: %s", strerror(errno));
        return ret;
    }

    assert(0 != frm_interval.discrete.denominator);

    *framerate = 1.0 / (1.0 * frm_interval.discrete.numerator / frm_interval.discrete.denominator);

    return 0;
}

int AtomISP::v4l2_capture_s_format(int fd, int device, int w, int h, int fourcc, bool raw, int* stride)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_format v4l2_fmt;
    CLEAR(v4l2_fmt);

    if (device == V4L2_INJECT_DEVICE) {
        v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        v4l2_fmt.fmt.pix.width = mFileInject.width;
        v4l2_fmt.fmt.pix.height = mFileInject.height;
        v4l2_fmt.fmt.pix.pixelformat = mFileInject.format;
        v4l2_fmt.fmt.pix.sizeimage = PAGE_ALIGN(mFileInject.size);
        v4l2_fmt.fmt.pix.priv = mFileInject.bayerOrder;

        LOG1("VIDIOC_S_FMT: device %d, width: %d, height: %d, format: %x, size: %d, bayer_order: %d",
                device,
                mFileInject.width,
                mFileInject.height,
                mFileInject.format,
                mFileInject.size,
                mFileInject.bayerOrder);
        ret = ioctl(fd, VIDIOC_S_FMT, &v4l2_fmt);
        if (ret < 0) {
            LOGE("VIDIOC_S_FMT failed: %s", strerror(errno));
            return -1;
        }
        return 0;
    }

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    LOG1("VIDIOC_G_FMT");
    ret = ioctl (fd,  VIDIOC_G_FMT, &v4l2_fmt);
    if (ret < 0) {
        LOGE("VIDIOC_G_FMT failed: %s", strerror(errno));
        return -1;
    }
    if (raw) {
        LOG1("Choose raw dump path");
        v4l2_fmt.type = V4L2_BUF_TYPE_PRIVATE;
    } else {
        v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }

    v4l2_fmt.fmt.pix.width = w;
    v4l2_fmt.fmt.pix.height = h;
    v4l2_fmt.fmt.pix.pixelformat = fourcc;
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;
    LOG1("VIDIOC_S_FMT: width: %d, height: %d, format: %d, field: %d",
                v4l2_fmt.fmt.pix.width,
                v4l2_fmt.fmt.pix.height,
                v4l2_fmt.fmt.pix.pixelformat,
                v4l2_fmt.fmt.pix.field);
    ret = ioctl(fd, VIDIOC_S_FMT, &v4l2_fmt);
    if (ret < 0) {
        LOGE("VIDIOC_S_FMT failed: %s", strerror(errno));
        return -1;
    }

    //get stride from ISP
    *stride = bytesPerLineToWidth(fourcc,v4l2_fmt.fmt.pix.bytesperline);
    LOG1("stride: %d from ISP", *stride);
    if (raw) {
        mRawDataDumpSize = v4l2_fmt.fmt.pix.priv;
        LOG1("raw data size from kernel %d\n", mRawDataDumpSize);
    }

    return 0;

}

int AtomISP::v4l2_capture_qbuf(int fd, int index, struct v4l2_buffer_info *buf)
{
    LOG2("@%s", __FUNCTION__);
    struct v4l2_buffer *v4l2_buf = &buf->vbuffer;
    int ret;

    if (fd < 0) //Device is closed
        return 0;
    ret = ioctl(fd, VIDIOC_QBUF, v4l2_buf);
    if (ret < 0) {
        LOGE("VIDIOC_QBUF index %d failed: %s",
             index, strerror(errno));
        return ret;
    }
    return ret;
}

status_t AtomISP::v4l2_capture_open(int device)
{
    LOG1("@%s", __FUNCTION__);
    VALID_DEVICE(device, INVALID_OPERATION);

    int fd;
    struct stat st;

    if ((device < V4L2_MAIN_DEVICE || device > mConfigLastDevice) && device != V4L2_ISP_SUBDEV) {
        LOGE("Wrong device node %d", device);
        return -1;
    }

    const char *dev_name;
    if (device == V4L2_ISP_SUBDEV)
        dev_name = PlatformData::getISPSubDeviceName();
    else
        dev_name = dev_name_array[device];
    LOG1("---Open video device %s---", dev_name);

    if (stat (dev_name, &st) == -1) {
        LOGE("Error stat video device %s: %s",
             dev_name, strerror(errno));
        return -1;
    }

    if (!S_ISCHR (st.st_mode)) {
        LOGE("%s is not a device", dev_name);
        return -1;
    }

    fd = open(dev_name, O_RDWR);

    if (fd <= 0) {
        LOGE("Error opening video device %s: %s",
            dev_name, strerror(errno));
        return -1;
    }

    return fd;
}

status_t AtomISP::v4l2_capture_close(int fd)
{
    LOG1("@%s", __FUNCTION__);
    /* close video device */
    LOG1("----close device ---");
    if (fd < 0) {
        LOGW("Device not opened!");
        return INVALID_OPERATION;
    }

    if (close(fd) < 0) {
        LOGE("Close video device failed: %s", strerror(errno));
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t AtomISP::v4l2_capture_querycap(int device, struct v4l2_capability *cap)
{
    LOG1("@%s", __FUNCTION__);
    int ret = 0;
    int fd = video_fds[device];

    ret = ioctl(fd, VIDIOC_QUERYCAP, cap);

    if (ret < 0) {
        LOGE("VIDIOC_QUERYCAP returned: %d (%s)", ret, strerror(errno));
        return ret;
    }

    if (device == V4L2_INJECT_DEVICE) {
        if (!(cap->capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
            LOGE("No output devices");
            return -1;
        }
        return ret;
    }

    if (!(cap->capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOGE("No capture devices");
        return -1;
    }

    if (!(cap->capabilities & V4L2_CAP_STREAMING)) {
        LOGE("Is not a video streaming device");
        return -1;
    }

    LOG1( "driver:      '%s'", cap->driver);
    LOG1( "card:        '%s'", cap->card);
    LOG1( "bus_info:      '%s'", cap->bus_info);
    LOG1( "version:      %x", cap->version);
    LOG1( "capabilities:      %x", cap->capabilities);

    return ret;
}

status_t AtomISP::v4l2_capture_s_input(int fd, int index)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_input input;
    int ret;

    LOG1("VIDIOC_S_INPUT");
    input.index = index;

    ret = ioctl(fd, VIDIOC_S_INPUT, &input);

    if (ret < 0) {
        LOGE("VIDIOC_S_INPUT index %d returned: %d (%s)",
            input.index, ret, strerror(errno));
        return ret;
    }
    return ret;
}

int AtomISP::atomisp_set_capture_mode(int deviceMode)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_streamparm parm;

    switch (deviceMode) {
    case CI_MODE_PREVIEW:
        LOG1("Setting CI_MODE_PREVIEW mode");
        break;;
    case CI_MODE_STILL_CAPTURE:
        LOG1("Setting CI_MODE_STILL_CAPTURE mode");
        break;
    case CI_MODE_VIDEO:
        LOG1("Setting CI_MODE_VIDEO mode");
        break;
    default:
        break;
    }

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.capturemode = deviceMode;
    if (ioctl(main_fd, VIDIOC_S_PARM, &parm) < 0) {
        LOGE("error %s", strerror(errno));
        return -1;
    }

    return 0;
}

int AtomISP::v4l2_capture_try_format(int device, int *w, int *h,
                                         int *fourcc)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    int fd = video_fds[device];
    struct v4l2_format v4l2_fmt;
    CLEAR(v4l2_fmt);

    if (device == V4L2_INJECT_DEVICE) {
        *w = mFileInject.width;
        *h = mFileInject.height;
        *fourcc = mFileInject.format;

        LOG1("width: %d, height: %d, format: %x, size: %d, bayer_order: %d",
             mFileInject.width,
             mFileInject.height,
             mFileInject.format,
             mFileInject.size,
             mFileInject.bayerOrder);

        return 0;
    }

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_fmt.fmt.pix.width = *w;
    v4l2_fmt.fmt.pix.height = *h;
    v4l2_fmt.fmt.pix.pixelformat = *fourcc;
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    ret = ioctl(fd, VIDIOC_TRY_FMT, &v4l2_fmt);
    if (ret < 0) {
        LOGE("VIDIOC_TRY_FMT returned: %d (%s)", ret, strerror(errno));
        return -1;
    }

    *w = v4l2_fmt.fmt.pix.width;
    *h = v4l2_fmt.fmt.pix.height;
    *fourcc = v4l2_fmt.fmt.pix.pixelformat;

    return 0;
}

/**
 * Pushes all preview buffers back into driver except the ones already queued
 *
 * Note: Currently no support for shared buffers for cautions
 */
status_t AtomISP::returnPreviewBuffers()
{
    LOG1("@%s", __FUNCTION__);
    status_t status;
    if (mPreviewBuffers) {
        for (int i = 0 ; i < mNumPreviewBuffers; i++) {
            if (mPreviewBuffers[i].shared)
                return UNKNOWN_ERROR;
            if (mPreviewBuffers[i].buff == NULL)
                return UNKNOWN_ERROR;
            // identifying already queued frames with negative id
            if (mPreviewBuffers[i].id == -1)
                continue;
            status = putPreviewFrame(&mPreviewBuffers[i]);
            if (status != NO_ERROR) {
                LOGE("Failed queueing preview frame!");
            }
        }
    }
    return NO_ERROR;
}

/**
 * Pushes all recording buffers back into driver except the ones already queued
 *
 * Note: Currently no support for shared buffers for cautions
 */
status_t AtomISP::returnRecordingBuffers()
{
    LOG1("@%s", __FUNCTION__);
    if (mRecordingBuffers) {
        for (int i = 0 ; i < mNumBuffers; i++) {
            if (mRecordingBuffers[i].shared)
                return UNKNOWN_ERROR;
            if (mRecordingBuffers[i].buff == NULL)
                return UNKNOWN_ERROR;
            // identifying already queued frames with negative id
            if (mRecordingBuffers[i].id == -1)
                continue;
            putRecordingFrame(&mRecordingBuffers[i]);
        }
    }
    return NO_ERROR;
}



status_t AtomISP::getPreviewFrame(AtomBuffer *buff, atomisp_frame_status *frameStatus)
{
    LOG2("@%s", __FUNCTION__);
    Mutex::Autolock lock(mDevices[mPreviewDevice].mutex);
    struct v4l2_buffer buf;

    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    int index = grabFrame(mPreviewDevice, &buf);
    if(index < 0){
        LOGE("Error in grabbing frame!");
        return BAD_INDEX;
    }
    LOG2("Device: %d. Grabbed frame of size: %d", mPreviewDevice, buf.bytesused);
    mPreviewBuffers[index].id = index;
    mPreviewBuffers[index].frameCounter = mDevices[mPreviewDevice].frameCounter;
    mPreviewBuffers[index].ispPrivate = mSessionId;
    mPreviewBuffers[index].capture_timestamp = buf.timestamp;
    // atom flag is an extended set of flags, so map V4L2 flags
    // we are interested into atomisp_frame_status
    if (buf.flags & V4L2_BUF_FLAG_ERROR)
        mPreviewBuffers[index].status = (FrameBufferStatus)ATOMISP_FRAME_STATUS_CORRUPTED;
    else
        mPreviewBuffers[index].status = (FrameBufferStatus)buf.reserved;

    if (frameStatus)
        *frameStatus = (atomisp_frame_status)mPreviewBuffers[index].status;
    *buff = mPreviewBuffers[index];

    mNumPreviewBuffersQueued--;

    dumpPreviewFrame(index);

    return NO_ERROR;
}

status_t AtomISP::putPreviewFrame(AtomBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    Mutex::Autolock lock(mDevices[mPreviewDevice].mutex);
    if (mMode == MODE_NONE)
        return INVALID_OPERATION;

    if ((buff->type == ATOM_BUFFER_PREVIEW) && (buff->ispPrivate != mSessionId))
        return DEAD_OBJECT;

    if (v4l2_capture_qbuf(video_fds[mPreviewDevice],
                      buff->id,
                      &v4l2_buf_pool[mPreviewDevice].bufs[buff->id]) < 0) {
        return UNKNOWN_ERROR;
    }

    // using -1 index to identify queued buffers
    // id gets updated with dqbuf
    mPreviewBuffers[buff->id].id = -1;

    mNumPreviewBuffersQueued++;

    return NO_ERROR;
}

/**
 * Override function for IBufferOwner
 *
 * Note: currently used only for preview
 */
void AtomISP::returnBuffer(AtomBuffer* buff)
{
    LOG2("@%s", __FUNCTION__);
    status_t status;
    if ((buff->type != ATOM_BUFFER_PREVIEW_GFX) &&
        (buff->type != ATOM_BUFFER_PREVIEW)) {
        LOGE("Received unexpected buffer!");
    } else {
        buff->owner = 0;
        status = putPreviewFrame(buff);
        if (status != NO_ERROR) {
            LOGE("Failed queueing preview frame!");
        }
    }
}

/**
 * Sets the externally allocated graphic buffers to be used
 * for the preview stream
 */
status_t AtomISP::setGraphicPreviewBuffers(const AtomBuffer *buffs, int numBuffs)
{
    LOG1("@%s: buffs = %p, numBuffs = %d", __FUNCTION__, buffs, numBuffs);
    if (buffs == NULL || numBuffs <= 0)
        return BAD_VALUE;

    if(mPreviewBuffers != NULL)
        freePreviewBuffers();

    mPreviewBuffers = new AtomBuffer[numBuffs];
    if (mPreviewBuffers == NULL)
        return NO_MEMORY;

    for (int i = 0; i < numBuffs; i++) {
        mPreviewBuffers[i] = buffs[i];
    }

    mNumPreviewBuffers = numBuffs;

    return NO_ERROR;
}

status_t AtomISP::getRecordingFrame(AtomBuffer *buff, nsecs_t *timestamp, atomisp_frame_status *frameStatus)
{
    LOG2("@%s", __FUNCTION__);
    struct v4l2_buffer buf;
    Mutex::Autolock lock(mDevices[mRecordingDevice].mutex);

    if (mMode != MODE_VIDEO)
        return INVALID_OPERATION;

    int index = grabFrame(mRecordingDevice, &buf);
    LOG2("index = %d", index);
    if(index < 0) {
        LOGE("Error in grabbing frame!");
        return BAD_INDEX;
    }
    LOG2("Device: %d. Grabbed frame of size: %d", mRecordingDevice, buf.bytesused);
    mRecordingBuffers[index].id = index;
    mRecordingBuffers[index].frameCounter = mDevices[mRecordingDevice].frameCounter;
    mRecordingBuffers[index].ispPrivate = mSessionId;
    mRecordingBuffers[index].capture_timestamp = buf.timestamp;
    *buff = mRecordingBuffers[index];
    buff->stride = mConfig.recording.stride;

    // time is get from ISP driver, it's realtime
    if (timestamp)
        *timestamp = (buf.timestamp.tv_sec)*1000000000LL + (buf.timestamp.tv_usec)*1000LL;

    if (frameStatus) {
        *frameStatus = (atomisp_frame_status)buf.reserved;

        // atom flag is an extended set of flags, so map V4L2 flags
        // we are interested into atomisp_frame_status
        if (buf.flags & V4L2_BUF_FLAG_ERROR)
            *frameStatus = ATOMISP_FRAME_STATUS_CORRUPTED;
    }

    mNumRecordingBuffersQueued--;

    dumpRecordingFrame(index);

    return NO_ERROR;
}

status_t AtomISP::putRecordingFrame(AtomBuffer *buff)
{
    LOG2("@%s", __FUNCTION__);
    Mutex::Autolock lock(mDevices[mRecordingDevice].mutex);

    if (mMode != MODE_VIDEO)
        return INVALID_OPERATION;

    if (buff->ispPrivate != mSessionId)
        return DEAD_OBJECT;

    if (v4l2_capture_qbuf(video_fds[mRecordingDevice],
            buff->id,
            &v4l2_buf_pool[mRecordingDevice].bufs[buff->id]) < 0) {
        return UNKNOWN_ERROR;
    }
    mRecordingBuffers[buff->id].id = -1;
    mNumRecordingBuffersQueued++;

    return NO_ERROR;
}

status_t AtomISP::setSnapshotBuffers(void *buffs, int numBuffs)
{
    LOG1("@%s: buffs = %p, numBuffs = %d", __FUNCTION__, buffs, numBuffs);
    if (buffs == NULL || numBuffs <= 0)
        return BAD_VALUE;

    mClientSnapshotBuffers = (void**)buffs;
    mConfig.num_snapshot = numBuffs;
    mUsingClientSnapshotBuffers = true;
    for (int i = 0; i < numBuffs; i++) {
        LOG1("Snapshot buffer %d = %p", i, mClientSnapshotBuffers[i]);
    }

    return NO_ERROR;
}

status_t AtomISP::getSnapshot(AtomBuffer *snapshotBuf, AtomBuffer *postviewBuf,
                              atomisp_frame_status *snapshotStatus)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_buffer buf;
    int snapshotIndex, postviewIndex;

    if (mMode != MODE_CAPTURE && mMode != MODE_CONTINUOUS_CAPTURE)
        return INVALID_OPERATION;

    snapshotIndex = grabFrame(V4L2_MAIN_DEVICE, &buf);
    if (snapshotIndex < 0) {
        LOGE("Error in grabbing frame from 1'st device!");
        return BAD_INDEX;
    }
    LOG1("Device: %d. Grabbed frame of size: %d", V4L2_MAIN_DEVICE, buf.bytesused);
    mSnapshotBuffers[snapshotIndex].capture_timestamp = buf.timestamp;
    mSnapshotBuffers[snapshotIndex].frameSequenceNbr = buf.sequence;

    if (snapshotStatus)
        *snapshotStatus = (atomisp_frame_status)buf.reserved;

    postviewIndex = grabFrame(V4L2_POSTVIEW_DEVICE, &buf);
    if (postviewIndex < 0) {
        LOGE("Error in grabbing frame from 2'nd device!");
        // If we failed with the second device, return the frame to the first device
        v4l2_capture_qbuf(video_fds[V4L2_MAIN_DEVICE], snapshotIndex,
                &v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[snapshotIndex]);
        return BAD_INDEX;
    }
    LOG1("Device: %d. Grabbed frame of size: %d", V4L2_POSTVIEW_DEVICE, buf.bytesused);
    mPostviewBuffers[postviewIndex].capture_timestamp = buf.timestamp;
    mPostviewBuffers[postviewIndex].frameSequenceNbr = buf.sequence;

    if (snapshotIndex != postviewIndex ||
            snapshotIndex >= MAX_V4L2_BUFFERS) {
        LOGE("Indexes error! snapshotIndex = %d, postviewIndex = %d", snapshotIndex, postviewIndex);
        // Return the buffers back to driver
        v4l2_capture_qbuf(video_fds[V4L2_MAIN_DEVICE], snapshotIndex,
                &v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[snapshotIndex]);
        v4l2_capture_qbuf(video_fds[V4L2_POSTVIEW_DEVICE], postviewIndex,
                &v4l2_buf_pool[V4L2_POSTVIEW_DEVICE].bufs[postviewIndex]);
        return BAD_INDEX;
    }

    mSnapshotBuffers[snapshotIndex].id = snapshotIndex;
    mSnapshotBuffers[snapshotIndex].frameCounter = mDevices[V4L2_MAIN_DEVICE].frameCounter;
    mSnapshotBuffers[snapshotIndex].ispPrivate = mSessionId;
    *snapshotBuf = mSnapshotBuffers[snapshotIndex];
    snapshotBuf->width = mConfig.snapshot.width;
    snapshotBuf->height = mConfig.snapshot.height;
    snapshotBuf->format = mConfig.snapshot.format;
    snapshotBuf->size = mConfig.snapshot.size;
    snapshotBuf->stride = mConfig.snapshot.stride;

    mPostviewBuffers[postviewIndex].id = postviewIndex;
    mPostviewBuffers[postviewIndex].frameCounter = mDevices[V4L2_POSTVIEW_DEVICE].frameCounter;
    mPostviewBuffers[postviewIndex].ispPrivate = mSessionId;
    *postviewBuf = mPostviewBuffers[postviewIndex];
    postviewBuf->width = mConfig.postview.width;
    postviewBuf->height = mConfig.postview.height;
    postviewBuf->format = mConfig.postview.format;
    postviewBuf->size = mConfig.postview.size;
    postviewBuf->stride = mConfig.postview.stride;

    mNumCapturegBuffersQueued--;

    dumpSnapshot(snapshotIndex, postviewIndex);

    return NO_ERROR;
}

status_t AtomISP::putSnapshot(AtomBuffer *snaphotBuf, AtomBuffer *postviewBuf)
{
    LOG1("@%s", __FUNCTION__);
    int ret0, ret1;

    if (mMode != MODE_CAPTURE && mMode != MODE_CONTINUOUS_CAPTURE)
        return INVALID_OPERATION;

    if (snaphotBuf->ispPrivate != mSessionId || postviewBuf->ispPrivate != mSessionId)
        return DEAD_OBJECT;

    ret0 = v4l2_capture_qbuf(video_fds[V4L2_MAIN_DEVICE], snaphotBuf->id,
                      &v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[snaphotBuf->id]);

    ret1 = v4l2_capture_qbuf(video_fds[V4L2_POSTVIEW_DEVICE], postviewBuf->id,
                      &v4l2_buf_pool[V4L2_POSTVIEW_DEVICE].bufs[postviewBuf->id]);
    if (ret0 < 0 || ret1 < 0)
        return UNKNOWN_ERROR;

    mNumCapturegBuffersQueued++;

    return NO_ERROR;
}

bool AtomISP::dataAvailable()
{
    LOG2("@%s", __FUNCTION__);

    // For video/recording, make sure isp has a preview and a recording buffer
    if (mMode == MODE_VIDEO)
        return mNumRecordingBuffersQueued > 0 && mNumPreviewBuffersQueued > 0;

    // For capture, just make sure isp has a capture buffer
    if (mMode == MODE_CAPTURE)
        return mNumCapturegBuffersQueued > 0;

    // For preview, just make sure isp has a preview buffer
    if (mMode == MODE_PREVIEW || mMode == MODE_CONTINUOUS_CAPTURE)
        return mNumPreviewBuffersQueued > 0;

    LOGE("Query for data in invalid mode");

    return false;
}

/**
 * Polls the preview device node fd for data
 *
 * \param timeout time to wait for data (in ms), timeout of -1
 *        means to wait indefinitely for data
 * \return -1 for error, 0 if time out, positive number
 *         if poll was successful
 */
int AtomISP::pollPreview(int timeout)
{
    LOG2("@%s", __FUNCTION__);
    return v4l2_poll(mPreviewDevice, timeout);
}

/**
 * Polls the preview device node fd for data
 *
 * \param timeout time to wait for data (in ms), timeout of -1
 *        means to wait indefinitely for data
 * \return -1 for error, 0 if time out, positive number
 *         if poll was successful
 */
int AtomISP::pollCapture(int timeout)
{
    LOG2("@%s", __FUNCTION__);
    return v4l2_poll(V4L2_MAIN_DEVICE, timeout);
}

bool AtomISP::isBufferValid(const AtomBuffer* buffer) const
{
    if(buffer->type == ATOM_BUFFER_PREVIEW_GFX)
        return true;

    return buffer->ispPrivate == this->mSessionId;
}

int AtomISP::grabFrame(int device, struct v4l2_buffer *buf)
{
    LOG2("@%s", __FUNCTION__);
    int ret;

    VALID_DEVICE(device, -1);

    ret = v4l2_capture_dqbuf(video_fds[device], buf);

    if (ret < 0)
        return ret;

    // inc frame counter but do no wrap to negative numbers
    ++mDevices[device].frameCounter;
    mDevices[device].frameCounter &= INT_MAX;

    return buf->index;
}

int AtomISP::v4l2_capture_dqbuf(int fd, struct v4l2_buffer *buf)
{
    LOG2("@%s", __FUNCTION__);
    int ret;

    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf->memory = V4L2_MEMORY_USERPTR;

    ret = ioctl(fd, VIDIOC_DQBUF, buf);

    if (ret < 0) {
        LOGE("error dequeuing buffers");
        return ret;
    }

    return buf->index;
}

int AtomISP::v4l2_subscribe_event(int fd, int event)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_event_subscription sub;

    memset(&sub, 0, sizeof(sub));
    sub.type = event;

    ret = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if (ret < 0) {
        LOGE("error subscribing event: %s", strerror(errno));
        return ret;
    }

    return ret;
}

int AtomISP::v4l2_unsubscribe_event(int fd, int event)
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    struct v4l2_event_subscription sub;

    memset(&sub, 0, sizeof(sub));
    sub.type = event;

    ret = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub);
    if (ret < 0) {
        LOGE("error unsubscribing event");
        return ret;
    }

    return ret;
}

int AtomISP::v4l2_dqevent(int fd, struct v4l2_event *event)
{
    LOG2("@%s", __FUNCTION__);
    int ret;

    ret = ioctl(fd, VIDIOC_DQEVENT, event);
    if (ret < 0) {
        LOGE("error dequeuing event");
        return ret;
    }

    return ret;
}

////////////////////////////////////////////////////////////////////
//                          PRIVATE METHODS
////////////////////////////////////////////////////////////////////

status_t AtomISP::allocatePreviewBuffers()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int allocatedBufs = 0;

    if (mPreviewBuffers == NULL) {
        mPreviewBuffers = new AtomBuffer[mNumPreviewBuffers];
        if (!mPreviewBuffers) {
            LOGE("Not enough mem for preview buffer array");
            status = NO_MEMORY;
            goto errorFree;
        }

        LOG1("Allocating %d buffers of size %d", mNumPreviewBuffers, mConfig.preview.size);
        for (int i = 0; i < mNumPreviewBuffers; i++) {
             mPreviewBuffers[i].buff = NULL;
             mPreviewBuffers[i].type = ATOM_BUFFER_PREVIEW;
             mPreviewBuffers[i].width = mConfig.preview.width;
             mPreviewBuffers[i].height = mConfig.preview.height;
             mPreviewBuffers[i].stride = mConfig.preview.stride;
             mPreviewBuffers[i].format = mConfig.preview.format;
             mCallbacks->allocateMemory(&mPreviewBuffers[i],  mConfig.preview.size);
             if (mPreviewBuffers[i].buff == NULL) {
                 LOGE("Error allocation memory for preview buffers!");
                 status = NO_MEMORY;
                 goto errorFree;
             }
             mPreviewBuffers[i].size = mConfig.preview.size;
             allocatedBufs++;
             v4l2_buf_pool[mPreviewDevice].bufs[i].data = mPreviewBuffers[i].buff->data;
             mPreviewBuffers[i].shared = false;
        }

    } else {
        for (int i = 0; i < mNumPreviewBuffers; i++) {
            v4l2_buf_pool[mPreviewDevice].bufs[i].data = mPreviewBuffers[i].gfxData;
            mPreviewBuffers[i].shared = true;
        }
    }

    return status;
errorFree:
    // On error, free the allocated buffers
    for (int i = 0 ; i < allocatedBufs; i++) {
        if (mPreviewBuffers[i].buff != NULL) {
            mPreviewBuffers[i].buff->release(mPreviewBuffers[i].buff);
            mPreviewBuffers[i].buff = NULL;
        }
    }
    if (mPreviewBuffers != NULL) {
        delete[] mPreviewBuffers;
        mPreviewBuffers = NULL;
    }

    return status;
}

status_t AtomISP::allocateRecordingBuffers()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int allocatedBufs = 0;
    int size;

    size = mConfig.recording.stride * mConfig.recording.height * 3 / 2;

    mRecordingBuffers = new AtomBuffer[mNumBuffers];
    if (!mRecordingBuffers) {
        LOGE("Not enough mem for recording buffer array");
        status = NO_MEMORY;
        goto errorFree;
    }

    for (int i = 0; i < mNumBuffers; i++) {
        mRecordingBuffers[i].buff = NULL;
        mRecordingBuffers[i].metadata_buff = NULL;
        // recording buffers use uncached memory
        mCallbacks->allocateMemory(&mRecordingBuffers[i], size, false);
        LOG1("allocate recording buffer[%d], buff=%p size=%d",
                i, mRecordingBuffers[i].buff->data, mRecordingBuffers[i].buff->size);
        if (mRecordingBuffers[i].buff == NULL) {
            LOGE("Error allocation memory for recording buffers!");
            status = NO_MEMORY;
            goto errorFree;
        }
        allocatedBufs++;
        v4l2_buf_pool[mRecordingDevice].bufs[i].data = mRecordingBuffers[i].buff->data;
        mRecordingBuffers[i].shared = false;
        mRecordingBuffers[i].width = mConfig.recording.width;
        mRecordingBuffers[i].height = mConfig.recording.height;
        mRecordingBuffers[i].size = mConfig.recording.size;
        mRecordingBuffers[i].stride = mConfig.recording.stride;
        mRecordingBuffers[i].format = mConfig.recording.format;
        mRecordingBuffers[i].type = ATOM_BUFFER_VIDEO;
    }
    return status;

errorFree:
    // On error, free the allocated buffers
    for (int i = 0 ; i < allocatedBufs; i++) {
        if (mRecordingBuffers[i].buff != NULL) {
            mRecordingBuffers[i].buff->release(mRecordingBuffers[i].buff);
            mRecordingBuffers[i].buff = NULL;
        }
    }
    if (mRecordingBuffers != NULL) {
        delete[] mRecordingBuffers;
        mRecordingBuffers = NULL;
    }
    return status;
}

status_t AtomISP::allocateSnapshotBuffers()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int allocatedSnaphotBufs = 0;
    int allocatedPostviewBufs = 0;
    int snapshotSize = mConfig.snapshot.size;

    if (mUsingClientSnapshotBuffers)
        snapshotSize = sizeof(void*);

    // note: make sure client has called releaseCaptureBuffers()
    //       at this point (clients may hold on to snapshot buffers
    //       after capture has been stopped)
    if (mSnapshotBuffers[0].buff != NULL) {
        LOGW("Client has not freed snapshot buffers!");
        freeSnapshotBuffers();
    }

    LOG1("Allocating %d buffers of size: %d (snapshot), %d (postview)",
            mConfig.num_snapshot,
            snapshotSize,
            mConfig.postview.size);
    for (int i = 0; i < mConfig.num_snapshot; i++) {
        mSnapshotBuffers[i].buff = NULL;
        mCallbacks->allocateMemory(&mSnapshotBuffers[i], snapshotSize);
        if (mSnapshotBuffers[i].buff == NULL) {
            LOGE("Error allocation memory for snapshot buffers!");
            status = NO_MEMORY;
            goto errorFree;
        }
        mSnapshotBuffers[i].type = ATOM_BUFFER_SNAPSHOT;
        allocatedSnaphotBufs++;
        if (mUsingClientSnapshotBuffers) {
            v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[i].data = mClientSnapshotBuffers[i];
            memcpy(mSnapshotBuffers[i].buff->data, &mClientSnapshotBuffers[i], sizeof(void *));
            mSnapshotBuffers[i].shared = true;

        } else {
            v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[i].data = mSnapshotBuffers[i].buff->data;
            mSnapshotBuffers[i].shared = false;
        }

        mPostviewBuffers[i].buff = NULL;
        mCallbacks->allocateMemory(&mPostviewBuffers[i], mConfig.postview.size);
        if (mPostviewBuffers[i].buff == NULL) {
            LOGE("Error allocation memory for postview buffers!");
            status = NO_MEMORY;
            goto errorFree;
        }
        mPostviewBuffers[i].type = ATOM_BUFFER_POSTVIEW;
        allocatedPostviewBufs++;
        v4l2_buf_pool[V4L2_POSTVIEW_DEVICE].bufs[i].data = mPostviewBuffers[i].buff->data;
        mPostviewBuffers[i].shared = false;
        mPostviewBuffers[i].stride = mConfig.postview.stride;
    }
    return status;

errorFree:
    // On error, free the allocated buffers
    for (int i = 0 ; i < allocatedSnaphotBufs; i++) {
        if (mSnapshotBuffers[i].buff != NULL) {
            mSnapshotBuffers[i].buff->release(mSnapshotBuffers[i].buff);
            mSnapshotBuffers[i].buff = NULL;
        }
    }
    for (int i = 0 ; i < allocatedPostviewBufs; i++) {
        if (mPostviewBuffers[i].buff != NULL) {
            mPostviewBuffers[i].buff->release(mPostviewBuffers[i].buff);
            mPostviewBuffers[i].buff = NULL;
        }
    }
    return status;
}

#ifdef ENABLE_INTEL_METABUFFER
void AtomISP::initMetaDataBuf(IntelMetadataBuffer* metaDatabuf)
{
    ValueInfo* vinfo = new ValueInfo;
    vinfo->mode = MEM_MODE_NONECACHE_USRPTR;
    vinfo->handle = 0;
    vinfo->width = mConfig.recording.width;
    vinfo->height = mConfig.recording.height;
    vinfo->size = mConfig.recording.size;
    //stride need to fill
    vinfo->lumaStride = mConfig.recording.stride;
    vinfo->chromStride = mConfig.recording.stride;
    LOG2("weight:%d  height:%d size:%d stride:%d ", vinfo->width,
          vinfo->height, vinfo->size, vinfo->lumaStride);
    vinfo->format = STRING_TO_FOURCC("NV12");
    vinfo->s3dformat = 0xFFFFFFFF;
    metaDatabuf->SetValueInfo(vinfo);
    delete vinfo;

}
#endif

status_t AtomISP::allocateMetaDataBuffers()
{
    LOG1("@%s", __FUNCTION__);

    status_t status = NO_ERROR;
#ifdef ENABLE_INTEL_METABUFFER
    int allocatedBufs = 0;
    uint8_t* meta_data_prt;
    uint32_t meta_data_size;
    IntelMetadataBuffer* metaDataBuf = NULL;

    if(mRecordingBuffers) {
        for (int i = 0 ; i < mNumBuffers; i++) {
            if (mRecordingBuffers[i].metadata_buff != NULL) {
                mRecordingBuffers[i].metadata_buff->release(mRecordingBuffers[i].metadata_buff);
                mRecordingBuffers[i].metadata_buff = NULL;
            }
        }
    } else {
        // mRecordingBuffers is not ready, so it's invalid to allocate metadata buffers
        return INVALID_OPERATION;
    }

    for (int i = 0; i < mNumBuffers; i++) {
        metaDataBuf = new IntelMetadataBuffer();
        if(metaDataBuf) {
            initMetaDataBuf(metaDataBuf);
            metaDataBuf->SetValue((uint32_t)mRecordingBuffers[i].buff->data);
            metaDataBuf->Serialize(meta_data_prt, meta_data_size);
            mRecordingBuffers[i].metadata_buff = NULL;
            mCallbacks->allocateMemory(&mRecordingBuffers[i].metadata_buff, meta_data_size);
            LOG1("allocate metadata buffer[%d]  buff=%p size=%d",
                i, mRecordingBuffers[i].metadata_buff->data,
                mRecordingBuffers[i].metadata_buff->size);
            if (mRecordingBuffers[i].metadata_buff == NULL) {
                LOGE("Error allocation memory for metadata buffers!");
                status = NO_MEMORY;
                goto errorFree;
            }
            memcpy(mRecordingBuffers[i].metadata_buff->data, meta_data_prt, meta_data_size);
            allocatedBufs++;

            delete metaDataBuf;
            metaDataBuf = NULL;
        } else {
            LOGE("Error allocation memory for metaDataBuf!");
            status = NO_MEMORY;
            goto errorFree;
        }
    }
    return status;

errorFree:
    // On error, free the allocated buffers
    if (mRecordingBuffers != NULL) {
        for (int i = 0 ; i < allocatedBufs; i++) {
            if (mRecordingBuffers[i].metadata_buff != NULL) {
                mRecordingBuffers[i].metadata_buff->release(mRecordingBuffers[i].metadata_buff);
                mRecordingBuffers[i].metadata_buff = NULL;
            }
        }
    }
    if (metaDataBuf) {
        delete metaDataBuf;
        metaDataBuf = NULL;
    }
#endif
    return status;
}

status_t AtomISP::freePreviewBuffers()
{
    LOG1("@%s", __FUNCTION__);
    if (mPreviewBuffers != NULL) {
        for (int i = 0 ; i < mNumPreviewBuffers; i++) {
            if (mPreviewBuffers[i].buff != NULL) {
                mPreviewBuffers[i].buff->release(mPreviewBuffers[i].buff);
                mPreviewBuffers[i].buff = NULL;
            }
        }
        delete [] mPreviewBuffers;
        mPreviewBuffers = NULL;
    }
    return NO_ERROR;
}

status_t AtomISP::freeRecordingBuffers()
{
    LOG1("@%s", __FUNCTION__);
    if(mRecordingBuffers != NULL) {
        for (int i = 0 ; i < mNumBuffers; i++) {
            if (mRecordingBuffers[i].buff != NULL) {
                mRecordingBuffers[i].buff->release(mRecordingBuffers[i].buff);
                mRecordingBuffers[i].buff = NULL;
            }
            if (mRecordingBuffers[i].metadata_buff != NULL) {
                mRecordingBuffers[i].metadata_buff->release(mRecordingBuffers[i].metadata_buff);
                mRecordingBuffers[i].metadata_buff = NULL;
            }
        }
        delete[] mRecordingBuffers;
        mRecordingBuffers = NULL;
    }
    return NO_ERROR;
}

status_t AtomISP::freeSnapshotBuffers()
{
    LOG1("@%s", __FUNCTION__);
    for (int i = 0 ; i < mConfig.num_snapshot; i++) {
        if (mSnapshotBuffers[i].buff != NULL) {
            mSnapshotBuffers[i].buff->release(mSnapshotBuffers[i].buff);
            mSnapshotBuffers[i].buff = NULL;
        }
        if (mPostviewBuffers[i].buff != NULL) {
            mPostviewBuffers[i].buff->release(mPostviewBuffers[i].buff);
            mPostviewBuffers[i].buff = NULL;
        }
    }
    return NO_ERROR;
}

int AtomISP::getNumberOfCameras()
{
    LOG1("@%s", __FUNCTION__);
    // note: hide the file inject device node, so do
    //       not allow to get info for MAX_CAMERA_NODES
    int nodes = PlatformData::numberOfCameras();
    if (nodes > MAX_CAMERAS)
        nodes = MAX_CAMERAS;
    return nodes;
}

size_t AtomISP::setupCameraInfo()
{
    LOG1("@%s", __FUNCTION__);
    int ret;
    size_t numCameras = 0;
    struct v4l2_input input;

    if (main_fd < 0)
        return numCameras;

    for (int i = 0; i < PlatformData::numberOfCameras(); i++) {
        memset(&input, 0, sizeof(input));
        memset(&sCamInfo[i], 0, sizeof(sCamInfo[i]));
        input.index = i;
        ret = ioctl(main_fd, VIDIOC_ENUMINPUT, &input);
        if (ret < 0) {
            sCamInfo[i].port = -1;
            LOGE("VIDIOC_ENUMINPUT failed for sensor input %d", i);
        } else {
            sCamInfo[i].port = input.reserved[1];
            sCamInfo[i].index = i;
            strncpy(sCamInfo[i].name, (const char *)input.name, sizeof(sCamInfo[i].name)-1);
            LOG1("Detected sensor \"%s\"", sCamInfo[i].name);
        }
        numCameras++;
    }
    return numCameras;
}

status_t AtomISP::getCameraInfo(int cameraId, camera_info *cameraInfo)
{
    LOG1("@%s: cameraId = %d", __FUNCTION__, cameraId);
    if (cameraId >= PlatformData::numberOfCameras())
        return BAD_VALUE;

    cameraInfo->facing = PlatformData::cameraFacing(cameraId);
    cameraInfo->orientation = PlatformData::cameraOrientation(cameraId);

    LOG1("@%s: %d: facing %s, orientation %d",
         __FUNCTION__,
         cameraId,
         ((cameraInfo->facing == CAMERA_FACING_BACK) ?
          "back" : "front/other"),
         cameraInfo->orientation);

    return NO_ERROR;
}

int AtomISP::getNumOfSkipFrames(void)
{
    int ret = 0;
    int num_skipframes = 0;

    ret = atomisp_get_attribute(main_fd, V4L2_CID_G_SKIP_FRAMES,
                                &num_skipframes);

    LOG1("%s: returns %d skip frame needed %d",__FUNCTION__, ret, num_skipframes);
    if (ret < 0)
        return ret;
    else
        return num_skipframes;
}

/* ===================  ACCELERATION API EXTENSIONS ====================== */
/*
* Loads the acceleration firmware to ISP. Calls the appropriate
* Driver IOCTL calls. Driver checks the validity of the firmware
* and fills the "fw_handle"
*/
int AtomISP::loadAccFirmware(void *fw, size_t size,
                             unsigned int *fwHandle)
{
    LOG1("@%s\n", __FUNCTION__);
    int ret = -1;

    //Load the IOCTL struct
    atomisp_acc_fw_load fwData;
    fwData.size = size;
    fwData.fw_handle = 0;
    fwData.data = fw;
    LOG2("fwData : 0x%x fwData->data : 0x%x",
        (unsigned int)&fwData, (unsigned int)fwData.data );



    if ( main_fd ){
        ret = xioctl(main_fd, ATOMISP_IOC_ACC_LOAD, &fwData);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_LOAD ret : %d fwData->fw_handle: %d \n"\
                , __FUNCTION__, ret, fwData.fw_handle);
    }

    //If IOCTRL call was returned successfully, get the firmware handle
    //from the structure and return it to the application.
    if(!ret){
        *fwHandle = fwData.fw_handle;
        LOG1("%s IOCTL Call returned : %d Handle: %ud\n",
                __FUNCTION__, ret, *fwHandle );
    }

    return ret;
}

int AtomISP::loadAccPipeFirmware(void *fw, size_t size,
                             unsigned int *fwHandle)
{
    LOG1("@%s", __FUNCTION__);
    int ret = -1;

    struct atomisp_acc_fw_load_to_pipe fwDataPipe;
    memset(&fwDataPipe, 0, sizeof(fwDataPipe));
    fwDataPipe.flags = ATOMISP_ACC_FW_LOAD_FL_PREVIEW;
    fwDataPipe.type = ATOMISP_ACC_FW_LOAD_TYPE_VIEWFINDER;

    /*  fwDataPipe.fw_handle filled by kernel and returned to caller */
    fwDataPipe.size = size;
    fwDataPipe.data = fw;

    if ( main_fd ){
        ret = xioctl(main_fd, ATOMISP_IOC_ACC_LOAD_TO_PIPE, &fwDataPipe);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_LOAD_TO_PIPE ret : %d fwDataPipe->fw_handle: %d"\
                , __FUNCTION__, ret, fwDataPipe.fw_handle);
    }

    //If IOCTRL call was returned successfully, get the firmware handle
    //from the structure and return it to the application.
    if(!ret){
        *fwHandle = fwDataPipe.fw_handle;
        LOG1("%s IOCTL Call returned : %d Handle: %ud",
                __FUNCTION__, ret, *fwHandle );
    }

    return ret;
}

/*
 * Unloads the acceleration firmware from ISP.
 * Atomisp driver checks the validity of the handles and schedules
 * unloading the firmware on the current frame complete. After this
 * call handle is not valid any more.
 */
int AtomISP::unloadAccFirmware(unsigned int fwHandle)
{
    LOG1("@ %s fw_Handle: %d\n",__FUNCTION__, fwHandle);
    int ret = -1;

    if ( main_fd ){
        ret = xioctl(main_fd, ATOMISP_IOC_ACC_UNLOAD, &fwHandle);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_UNLOAD ret: %d \n",
                __FUNCTION__,ret);
    }

    return ret;
}

int AtomISP::mapFirmwareArgument(void *val, size_t size, unsigned long *ptr)
{
    int ret = -1;
    struct atomisp_acc_map map;

    memset(&map, 0, sizeof(map));

    map.length = size;
    map.user_ptr = val;

    if ( main_fd ) {
        ret = ioctl(main_fd, ATOMISP_IOC_ACC_MAP, &map);
        LOG1("%s ATOMISP_IOC_ACC_MAP ret: %d\n",
                __FUNCTION__, ret);
    }

    *ptr = map.css_ptr;

    return ret;
}

int AtomISP::unmapFirmwareArgument(unsigned long val, size_t size)
{
    int ret = -1;
    struct atomisp_acc_map map;

    memset(&map, 0, sizeof(map));

    map.css_ptr = val;
    map.length = size;

    if ( main_fd ) {
        ret = ioctl(main_fd, ATOMISP_IOC_ACC_UNMAP, &map);
        LOG1("%s ATOMISP_IOC_ACC_UNMAP ret: %d\n",
                __func__, ret);
    }

    return ret;
}

/*
 * Sets the arguments for the firmware loaded.
 * The loaded firmware is identified with the firmware handle.
 * Atomisp driver checks the validity of the handle.
 */
int AtomISP::setFirmwareArgument(unsigned int fwHandle, unsigned int num,
                                 void *val, size_t size)
{
    LOG1("@ %s fwHandle:%d\n", __FUNCTION__, fwHandle);
    int ret = -1;

    atomisp_acc_fw_arg arg;
    arg.fw_handle = fwHandle;
    arg.index = num;
    arg.value = val;
    arg.size = size;

    if ( main_fd ){
        ret = xioctl(main_fd, ATOMISP_IOC_ACC_S_ARG, &arg);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_S_ARG ret: %d \n",
                __FUNCTION__, ret);
    }

    return ret;
}

int AtomISP::setMappedFirmwareArgument(unsigned int fwHandle, unsigned int mem,
                                       unsigned long val, size_t size)
{
    int ret = -1;
    struct atomisp_acc_s_mapped_arg arg;

    memset(&arg, 0, sizeof(arg));

    arg.fw_handle = fwHandle;
    arg.memory = mem;
    arg.css_ptr = val;
    arg.length = size;

    if ( main_fd ) {
        ret = ioctl(main_fd, ATOMISP_IOC_ACC_S_MAPPED_ARG, &arg);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_S_MAPPED_ARG ret: %d \n",
                __func__, ret);
    }

    return ret;
}

/*
 * For a stable argument, mark it is destabilized, i.e. flush it
 * was changed from user space and needs flushing from the cache
 * to provide CSS access to it.
 * The loaded firmware is identified with the firmware handle.
 * Atomisp driver checks the validity of the handle.
 */
int AtomISP::unsetFirmwareArgument(unsigned int fwHandle, unsigned int num)
{
    LOG1("@ %s fwHandle:%d", __FUNCTION__, fwHandle);
    int ret = -1;

    atomisp_acc_fw_arg arg;
    arg.fw_handle = fwHandle;
    arg.index = num;
    arg.value = NULL;
    arg.size = 0;

    if ( main_fd ){
        ret = xioctl(main_fd, ATOMISP_IOC_ACC_DESTAB, &arg);
        LOG1("%s IOCTL ATOMISP_IOC_ACC_DESTAB ret: %d \n",
                __FUNCTION__, ret);
    }

    return ret;
}

int AtomISP::startFirmware(unsigned int fwHandle)
{
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_ACC_START, &fwHandle);
    LOG1("%s IOCTL ATOMISP_IOC_ACC_START ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::waitForFirmware(unsigned int fwHandle)
{
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_ACC_WAIT, &fwHandle);
    LOG1("%s IOCTL ATOMISP_IOC_ACC_WAIT ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::abortFirmware(unsigned int fwHandle, unsigned int timeout)
{
    int ret;
    atomisp_acc_fw_abort abort;

    abort.fw_handle = fwHandle;
    abort.timeout = timeout;

    ret = xioctl(main_fd, ATOMISP_IOC_ACC_ABORT, &abort);
    LOG1("%s IOCTL ATOMISP_IOC_ACC_ABORT ret: %d\n", __FUNCTION__, ret);
    return ret;
}

status_t AtomISP::storeMetaDataInBuffers(bool enabled)
{
    LOG1("@%s: enabled = %d", __FUNCTION__, enabled);
    status_t status = NO_ERROR;
    mStoreMetaDataInBuffers = enabled;

    /**
     * if we are not in video mode we just store the value
     * it will be used during preview start
     * if we are in video mode we can allocate the buffers
     * now and start using them
     */
    if (mStoreMetaDataInBuffers && mMode == MODE_VIDEO) {
      if ((status = allocateMetaDataBuffers()) != NO_ERROR)
          goto exitFreeRec;
    }
    return status;

exitFreeRec:
    LOGE("Error allocating metadata buffers!");
    if(mRecordingBuffers) {
        for (int i = 0 ; i < mNumBuffers; i++) {
            if (mRecordingBuffers[i].metadata_buff != NULL) {
                mRecordingBuffers[i].metadata_buff->release(mRecordingBuffers[i].metadata_buff);
                mRecordingBuffers[i].metadata_buff = NULL;
            }
        }
    }
    return status;
}

int AtomISP::dumpPreviewFrame(int previewIndex)
{
    LOG2("@%s", __FUNCTION__);

    if (CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_PREVIEW)) {
        CameraDump *cameraDump = CameraDump::getInstance();
        const struct v4l2_buffer_info *buf =
            &v4l2_buf_pool[mPreviewDevice].bufs[previewIndex];
        if (mConfigRecordingPreviewDevice == mPreviewDevice)
            cameraDump->dumpImage2File(buf->data, mConfig.preview.size, mConfig.preview.width,
                                       mConfig.preview.height, DUMPIMAGE_RECORD_PREVIEW_FILENAME);
        else
            cameraDump->dumpImage2File(buf->data, mConfig.preview.size, mConfig.preview.width,
                                       mConfig.preview.height, DUMPIMAGE_PREVIEW_FILENAME);
    }

    return 0;
}

int AtomISP::dumpRecordingFrame(int recordingIndex)
{
    LOG2("@%s", __FUNCTION__);
    if (CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_VIDEO)) {
        CameraDump *cameraDump = CameraDump::getInstance();
        const struct v4l2_buffer_info *buf =
            &v4l2_buf_pool[mRecordingDevice].bufs[recordingIndex];
        const char *name = DUMPIMAGE_RECORD_STORE_FILENAME;
        cameraDump->dumpImage2File(buf->data, mConfig.recording.size, mConfig.recording.width,
                                   mConfig.recording.height, name);
    }

    return 0;
}

int AtomISP::dumpSnapshot(int snapshotIndex, int postviewIndex)
{
    LOG2("@%s", __FUNCTION__);
    if (CameraDump::isDumpImageEnable()) {
        CameraDump *cameraDump = CameraDump::getInstance();
        if (CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_SNAPSHOT)) {
           const struct v4l2_buffer_info *buf0 =
               &v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[snapshotIndex];
           const struct v4l2_buffer_info *buf1 =
               &v4l2_buf_pool[V4L2_POSTVIEW_DEVICE].bufs[postviewIndex];
           const char *name0 = "snap_v0.nv12";
           const char *name1 = "snap_v1.nv12";
           cameraDump->dumpImage2File(buf0->data, mConfig.snapshot.size, mConfig.snapshot.width,
                                      mConfig.snapshot.height, name0);
           cameraDump->dumpImage2File(buf1->data, mConfig.postview.size, mConfig.postview.width,
                                      mConfig.postview.height, name1);
        }

        if (CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_YUV)) {
            const struct v4l2_buffer_info *buf =
                &v4l2_buf_pool[V4L2_MAIN_DEVICE].bufs[snapshotIndex];
            cameraDump->dumpImage2Buf(buf->data, mConfig.snapshot.size, mConfig.snapshot.width,
                                      mConfig.snapshot.height);
        }

        if (isDumpRawImageReady()) {
            LOG1("dumping raw data");
            void *start = mmap(NULL /* start anywhere */ ,
                               PAGE_ALIGN(mRawDataDumpSize),
                               PROT_READ | PROT_WRITE /* required */ ,
                               MAP_SHARED /* recommended */ ,
                               video_fds[V4L2_MAIN_DEVICE], 0xfffff000);
            if (MAP_FAILED == start)
                    LOGE("mmap failed");
            else {
                LOG1("MMAP raw address from kernel 0x%p", start);
                cameraDump->dumpImage2Buf(start, mRawDataDumpSize, mConfig.snapshot.stride,
                                          mConfig.snapshot.height);
                if (-1 == munmap(start, PAGE_ALIGN(mRawDataDumpSize)))
                    LOGE("munmap failed");
            }
        }
    }

    return 0;
}

int AtomISP::dumpRawImageFlush(void)
{
    LOG1("@%s", __FUNCTION__);
    if (CameraDump::isDumpImageEnable()) {
        CameraDump *cameraDump = CameraDump::getInstance();
        cameraDump->dumpImage2FileFlush();
    }
    return 0;
}

bool AtomISP::isDumpRawImageReady(void)
{
    LOG1("@%s", __FUNCTION__);
    return (mSensorType == SENSOR_TYPE_RAW) && CameraDump::isDumpImageEnable(CAMERA_DEBUG_DUMP_RAW);
}

int AtomISP::sensorMoveFocusToPosition(int position)
{
    LOG2("@%s", __FUNCTION__);
#if MERR_VV
    position = 1024 - position;
    position = 100 + (position - 370) * 1.7;
    if(position > 900)
        position = 900;
    if (position < 100)
        position = 100;
#endif
    return atomisp_set_attribute(main_fd, V4L2_CID_FOCUS_ABSOLUTE, position, "Set focus position");
}

int AtomISP::sensorMoveFocusToBySteps(int steps)
{
    LOG2("@%s", __FUNCTION__);
    return atomisp_set_attribute(main_fd, V4L2_CID_FOCUS_RELATIVE, steps, "Set focus steps");
}

int AtomISP::sensorGetFocusStatus(int *status)
{
    LOG2("@%s", __FUNCTION__);
    return atomisp_get_attribute(main_fd,V4L2_CID_FOCUS_STATUS, status);
}

int AtomISP::sensorGetModeInfo(struct atomisp_sensor_mode_data *mode_data)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_G_SENSOR_MODE_DATA, mode_data);
    LOG2("%s IOCTL ATOMISP_IOC_G_SENSOR_MODE_DATA ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::sensorSetExposure(struct atomisp_exposure *exposure)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_EXPOSURE, exposure);
    LOG2("%s IOCTL ATOMISP_IOC_S_EXPOSURE ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::sensorGetExposureTime(int *time)
{
    LOG2("@%s", __FUNCTION__);
    return atomisp_get_attribute(main_fd, V4L2_CID_EXPOSURE_ABSOLUTE, time);
}

int AtomISP::sensorGetAperture(int *aperture)
{
    LOG2("@%s", __FUNCTION__);
    return atomisp_get_attribute(main_fd, V4L2_CID_IRIS_ABSOLUTE, aperture);
}

int AtomISP::sensorGetFNumber(unsigned short *fnum_num, unsigned short *fnum_denom)
{
    LOG2("@%s", __FUNCTION__);
    int fnum = 0, ret;

    ret = atomisp_get_attribute(main_fd, V4L2_CID_FNUMBER_ABSOLUTE, &fnum);

    *fnum_num = (unsigned short)(fnum >> 16);
    *fnum_denom = (unsigned short)(fnum & 0xFFFF);
    return ret;
}

void AtomISP::getSensorDataFromFile(const char *file_name, sensorPrivateData *sensor_data)
{
    LOG2("@%s", __FUNCTION__);
    int otp_fd = -1;
    struct stat st;
    struct v4l2_private_int_data otpdata;

    otpdata.size = 0;
    otpdata.data = NULL;
    otpdata.reserved[0] = 0;
    otpdata.reserved[1] = 0;

    sensor_data->data = NULL;
    sensor_data->size = 0;

    /* Open the otp data file */
    if ((otp_fd = open(file_name, O_RDONLY)) == -1) {
        LOGE("ERR(%s): Failed to open %s\n", __func__, file_name);
        return;
    }

    memset(&st, 0, sizeof (st));
    if (fstat(otp_fd, &st) < 0) {
        LOGE("ERR(%s): fstat %s failed\n", __func__, file_name);
        return;
    }

    otpdata.size = st.st_size;
    otpdata.data = malloc(otpdata.size);
    if (otpdata.data == NULL) {
        LOGD("Failed to allocate memory for OTP data.");
        return;
    }

    if ( (read(otp_fd, otpdata.data, otpdata.size)) == -1) {
        LOGD("Failed to read OTP data\n");
        free(otpdata.data);
        close(otp_fd);
        return;
    }

    sensor_data->data = otpdata.data;
    sensor_data->size = otpdata.size;
    close(otp_fd);
}

void AtomISP::sensorGetMotorData(sensorPrivateData *sensor_data)
{
    LOG2("@%s", __FUNCTION__);
    int rc;
    struct v4l2_private_int_data motorPrivateData;

    motorPrivateData.size = 0;
    motorPrivateData.data = NULL;
    motorPrivateData.reserved[0] = 0;
    motorPrivateData.reserved[1] = 0;

    sensor_data->data = NULL;
    sensor_data->size = 0;
    // First call with size = 0 will return motor private data size.
    rc = xioctl (main_fd, ATOMISP_IOC_G_MOTOR_PRIV_INT_DATA, &motorPrivateData);
    LOG2("%s IOCTL ATOMISP_IOC_G_MOTOR_PRIV_INT_DATA to get motor private data size ret: %d\n", __FUNCTION__, rc);
    if (rc != 0 || motorPrivateData.size == 0) {
        LOGD("Failed to get motor private data size. Error: %d", rc);
        return;
    }

    motorPrivateData.data = malloc(motorPrivateData.size);
    if (motorPrivateData.data == NULL) {
        LOGD("Failed to allocate memory for motor private data.");
        return;
    }

    // Second call with correct size will return motor private data.
    rc = xioctl (main_fd, ATOMISP_IOC_G_MOTOR_PRIV_INT_DATA, &motorPrivateData);
    LOG2("%s IOCTL ATOMISP_IOC_G_MOTOR_PRIV_INT_DATA to get motor private data ret: %d\n", __FUNCTION__, rc);

    if (rc != 0 || motorPrivateData.size == 0) {
        LOGD("Failed to read motor private data. Error: %d", rc);
        free(motorPrivateData.data);
        return;
    }

    sensor_data->data = motorPrivateData.data;
    sensor_data->size = motorPrivateData.size;
}

void AtomISP::sensorGetSensorData(sensorPrivateData *sensor_data)
{
    LOG2("@%s", __FUNCTION__);
    int rc;
    struct v4l2_private_int_data otpdata;

    otpdata.size = 0;
    otpdata.data = NULL;
    otpdata.reserved[0] = 0;
    otpdata.reserved[1] = 0;

    sensor_data->data = NULL;
    sensor_data->size = 0;
    // First call with size = 0 will return OTP data size.
    rc = xioctl (main_fd, ATOMISP_IOC_G_SENSOR_PRIV_INT_DATA, &otpdata);
    LOG2("%s IOCTL ATOMISP_IOC_G_SENSOR_PRIV_INT_DATA to get OTP data size ret: %d\n", __FUNCTION__, rc);
    if (rc != 0 || otpdata.size == 0) {
        LOGD("Failed to get OTP size. Error: %d", rc);
        return;
    }

    otpdata.data = malloc(otpdata.size);
    if (otpdata.data == NULL) {
        LOGD("Failed to allocate memory for OTP data.");
        return;
    }

    // Second call with correct size will return OTP data.
    rc = xioctl (main_fd, ATOMISP_IOC_G_SENSOR_PRIV_INT_DATA, &otpdata);
    LOG2("%s IOCTL ATOMISP_IOC_G_SENSOR_PRIV_INT_DATA to get OTP data ret: %d\n", __FUNCTION__, rc);

    if (rc != 0 || otpdata.size == 0) {
        LOGD("Failed to read OTP data. Error: %d", rc);
        free(otpdata.data);
        return;
    }

    sensor_data->data = otpdata.data;
    sensor_data->size = otpdata.size;
}

int AtomISP::setAicParameter(struct atomisp_parameters *aic_param)
{
    LOG2("@%s", __FUNCTION__);
    int ret;

#if MERR_VV
   aic_param->ctc_table = NULL;
   aic_param->gamma_table = NULL;
#endif

    ret = xioctl(main_fd, ATOMISP_IOC_S_PARAMETERS, aic_param);
    LOG2("%s IOCTL ATOMISP_IOC_S_PARAMETERS ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setIspParameter(struct atomisp_parm *isp_param)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_PARM, isp_param);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_PARM ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::getIspStatistics(struct atomisp_3a_statistics *statistics)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_G_3A_STAT, statistics);
    LOG2("%s IOCTL ATOMISP_IOC_G_3A_STAT ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setMaccConfig(struct atomisp_macc_config *macc_tbl)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_MACC,macc_tbl);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_MACC ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setFpnTable(struct v4l2_framebuffer *fb)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_FPN_TABLE, fb);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_FPN_TABLE ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setGammaTable(const struct atomisp_gamma_table *gamma_tbl)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_GAMMA, (struct atomisp_gamma_table *)gamma_tbl);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_GAMMA ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setCtcTable(const struct atomisp_ctc_table *ctc_tbl)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_CTC, (struct atomisp_ctc_table *)ctc_tbl);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_CTC ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setGdcConfig(const struct atomisp_morph_table *tbl)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_GDC_TAB, (struct atomisp_morph_table *)tbl);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_GDC_TAB ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setShadingTable(struct atomisp_shading_table *table)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_SHD_TAB, table);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_SHD_TAB ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setDeConfig(struct atomisp_de_config *de_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd,ATOMISP_IOC_S_ISP_FALSE_COLOR_CORRECTION, de_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_FALSE_COLOR_CORRECTION ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setTnrConfig(struct atomisp_tnr_config *tnr_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_TNR, tnr_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_TNR ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setEeConfig(struct atomisp_ee_config *ee_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_EE, ee_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_EE ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setNrConfig(struct atomisp_nr_config *nr_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_NR, nr_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_NR ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setDpConfig(struct atomisp_dp_config *dp_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_BAD_PIXEL_DETECTION, dp_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_BAD_PIXEL_DETECTION ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setWbConfig(struct atomisp_wb_config *wb_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_WHITE_BALANCE, wb_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_WHITE_BALANCE ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::set3aConfig(const struct atomisp_3a_config *cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_3A_CONFIG, (struct atomisp_3a_config *)cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_3A_CONFIG ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setObConfig(struct atomisp_ob_config *ob_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_BLACK_LEVEL_COMP, ob_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_BLACK_LEVEL_COMP ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setGcConfig(const struct atomisp_gc_config *gc_cfg)
{
    LOG2("@%s", __FUNCTION__);
    int ret;
    ret = xioctl(main_fd, ATOMISP_IOC_S_ISP_GAMMA_CORRECTION, (struct atomisp_gc_config *)gc_cfg);
    LOG2("%s IOCTL ATOMISP_IOC_S_ISP_GAMMA_CORRECTION ret: %d\n", __FUNCTION__, ret);
    return ret;
}

int AtomISP::setFlashIntensity(int intensity)
{
    LOG2("@%s", __FUNCTION__);
    return atomisp_set_attribute(main_fd, V4L2_CID_FLASH_INTENSITY, intensity, "Set flash intensity");
}

/**
 * TODO: deprecated, utilize observer. See mFrameSyncSource
 */
status_t AtomISP::enableFrameSyncEvent(bool enable)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    if (enable) {
        mFrameSyncRequested++;
    } else if (mFrameSyncRequested > 0) {
        mFrameSyncRequested--;
    }
    return status;
}

/**
 * Return IObserverSubject for ObserverType
 */
IObserverSubject* AtomISP::observerSubjectByType(ObserverType t)
{
    IObserverSubject *s = NULL;
    switch (t) {
        case OBSERVE_PREVIEW_STREAM:
            s = &mPreviewStreamSource;
            break;
        case OBSERVE_FRAME_SYNC_SOF:
            s = &mFrameSyncSource;
            break;
        default:
            break;
    }
    return s;
}

/**
 * Attaches observer to one of the defined ObserverType's
 */
status_t AtomISP::attachObserver(IAtomIspObserver *observer, ObserverType t)
{
    if (t == OBSERVE_FRAME_SYNC_SOF)
        mFrameSyncRequested++;

    return mObserverManager.attachObserver(observer, observerSubjectByType(t));
}

/**
 * Detaches observer from one of the defined ObserverType's
 */
status_t AtomISP::detachObserver(IAtomIspObserver *observer, ObserverType t)
{
    IObserverSubject *s = observerSubjectByType(t);
    status_t ret;

    ret = mObserverManager.detachObserver(observer, s);
    if (ret != NO_ERROR) {
        LOGE("%s failed!", __FUNCTION__);
        return ret;
    }

    if (t == OBSERVE_FRAME_SYNC_SOF) {
        if (--mFrameSyncRequested <= 0 && mFrameSyncEnabled) {
            v4l2_unsubscribe_event(video_fds[V4L2_ISP_SUBDEV], V4L2_EVENT_FRAME_SYNC);
            closeDevice(V4L2_ISP_SUBDEV);
            mFrameSyncEnabled = false;
            mFrameSyncRequested = 0;
        }
    }

    return ret;
}

/**
 * Pause and synchronise with observer
 *
 * Ability to sync into paused state is provided specifically
 * for ControlThread::stopPreviewCore() and OBSERVE_PREVIEW_STREAM.
 * This is for the sake of retaining the old semantics with buffers
 * flushing and keeping the initial preview parallelization changes
 * minimal (Bug 76149)
 *
 * Effectively this call blocks until observer method returns
 * normally and and then allows client to continue with the original
 * flow of flushing messages, AtomISP::stop() and release of buffers.
 *
 * TODO: The intention might rise e.g to optimize shutterlag
 * with CSS1.0 by streaming of the device and letting observers
 * to handle end-of-stream themself. Ideally, this can be done
 * with the help from IAtomIspObserver notify messages, but it
 * would require separating the stopping of threads operations
 * on buffers, the device streamoff and the release of buffers.
 */
void AtomISP::pauseObserver(ObserverType t)
{
    mObserverManager.setState(OBSERVER_STATE_PAUSED,
            observerSubjectByType(t), true);
}

/**
 * polls and dequeues SOF event into IAtomIspObserver::Message
 */
status_t AtomISP::FrameSyncSource::observe(IAtomIspObserver::Message *msg)
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_event event;
    int ret;

    if (!mISP->mFrameSyncEnabled) {
        msg->id = IAtomIspObserver::MESSAGE_ID_ERROR;
        LOGE("Frame sync not enabled");
        return INVALID_OPERATION;
    }

    ret = mISP->v4l2_poll(mISP->video_fds[V4L2_ISP_SUBDEV], FRAME_SYNC_POLL_TIMEOUT);

    if (ret <= 0) {
        LOGE("Poll failed, disabling SOF event");
        mISP->v4l2_unsubscribe_event(mISP->video_fds[V4L2_ISP_SUBDEV], V4L2_EVENT_FRAME_SYNC);
        mISP->closeDevice(V4L2_ISP_SUBDEV);
        mISP->mFrameSyncEnabled = false;
        msg->id = IAtomIspObserver::MESSAGE_ID_ERROR;
        return UNKNOWN_ERROR;
    }

    // poll was successful, dequeue the event right away
    do {
        ret = mISP->v4l2_dqevent(mISP->video_fds[V4L2_ISP_SUBDEV], &event);
        if (ret < 0) {
            LOGE("Dequeue event failed");
            msg->id = IAtomIspObserver::MESSAGE_ID_ERROR;
            return UNKNOWN_ERROR;
        }
    } while (event.pending > 0);

    // fill observer message
    msg->id = IAtomIspObserver::MESSAGE_ID_EVENT;
    msg->data.event.timestamp.tv_sec  = event.timestamp.tv_sec;
    msg->data.event.timestamp.tv_usec = event.timestamp.tv_nsec / 1000;
    msg->data.event.sequence = event.sequence;

    return NO_ERROR;
}

/**
 * polls and dequeues a preview frame into IAtomIspObserver::Message
 */
status_t AtomISP::PreviewStreamSource::observe(IAtomIspObserver::Message *msg)
{
    status_t status;
    int ret;
    LOG2("@%s", __FUNCTION__);
    int failCounter = 0;

try_again:
    ret = mISP->pollPreview(ATOMISP_PREVIEW_POLL_TIMEOUT);
    if (ret > 0) {
        LOG2("Entering dequeue : num-of-buffers queued %d", mISP->mNumPreviewBuffersQueued);
        status = mISP->getPreviewFrame(&msg->data.frameBuffer.buff);
        if (status != NO_ERROR) {
            msg->id = IAtomIspObserver::MESSAGE_ID_ERROR;
            status = UNKNOWN_ERROR;
        } else {
            msg->data.frameBuffer.buff.owner = mISP;
            msg->id = IAtomIspObserver::MESSAGE_ID_FRAME;
        }
    } else {
        LOGE("v4l2_poll for preview device failed! (%s)", (ret==0)?"timeout":"error");
        msg->id = IAtomIspObserver::MESSAGE_ID_ERROR;
        status = (ret==0)?TIMED_OUT:UNKNOWN_ERROR;
    }

    if (status != NO_ERROR) {
        // check if reason is starving and enter sleep to wait
        // for returnBuffer()
        while(!mISP->dataAvailable()) {
            if (++failCounter > ATOMISP_GETFRAME_RETRY_COUNT) {
                LOGD("There were no preview buffers returned in time");
                break;
            }
            LOGW("Preview stream starving from buffers!");
            usleep(ATOMISP_GETFRAME_STARVING_WAIT);
        }

        if (++failCounter <= ATOMISP_GETFRAME_RETRY_COUNT)
            goto try_again;
    }

    return status;
}

/**
 * TODO: deprecated, utilize observer. See mFrameSyncSource.
 */
int AtomISP::pollFrameSyncEvent()
{
    LOG1("@%s", __FUNCTION__);
    struct v4l2_event event;
    int ret;

    if (!mFrameSyncEnabled) {
        LOGE("Frame sync not enabled");
        return INVALID_OPERATION;
    }

    ret = v4l2_poll(video_fds[V4L2_ISP_SUBDEV], FRAME_SYNC_POLL_TIMEOUT);

    if (ret <= 0) {
        LOGE("Poll failed, disabling SOF event");
        v4l2_unsubscribe_event(video_fds[V4L2_ISP_SUBDEV], V4L2_EVENT_FRAME_SYNC);
        closeDevice(V4L2_ISP_SUBDEV);
        mFrameSyncEnabled = false;
        return UNKNOWN_ERROR;
    }

    // poll was successful, dequeue the event right away
    do {
        ret = v4l2_dqevent(video_fds[V4L2_ISP_SUBDEV], &event);
        if (ret < 0) {
            LOGE("Dequeue event failed");
            return UNKNOWN_ERROR;
        }
    } while (event.pending > 0);

    return NO_ERROR;
}

// I3AControls

status_t AtomISP::init3A()
{
    return NO_ERROR;
}

status_t AtomISP::deinit3A()
{
    return NO_ERROR;
}

status_t AtomISP::setAeMode(AeMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_exposure_auto_type v4lMode;

    // TODO: add supported modes to PlatformData
    if (getCurrentCameraId() > 0) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AE_MODE_AUTO:
            v4lMode = V4L2_EXPOSURE_AUTO;
            break;
        case CAM_AE_MODE_MANUAL:
            v4lMode = V4L2_EXPOSURE_MANUAL;
            break;
        case CAM_AE_MODE_SHUTTER_PRIORITY:
            v4lMode = V4L2_EXPOSURE_SHUTTER_PRIORITY;
            break;
        case CAM_AE_MODE_APERTURE_PRIORITY:
            v4lMode = V4L2_EXPOSURE_APERTURE_PRIORITY;
            break;
        default:
            LOGW("Unsupported AE mode (%d), using AUTO", mode);
            v4lMode = V4L2_EXPOSURE_AUTO;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_EXPOSURE_AUTO, v4lMode, "AE mode");
    if (ret != 0) {
        LOGE("Error setting AE mode (%d) in the driver", v4lMode);
        status = UNKNOWN_ERROR;
    }

    return status;
}

AeMode AtomISP::getAeMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    AeMode mode = CAM_AE_MODE_NOT_SET;
    v4l2_exposure_auto_type v4lMode = V4L2_EXPOSURE_AUTO;

    // TODO: add supported modes to PlatformData
    if (getCurrentCameraId() > 0) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_EXPOSURE_AUTO, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting AE mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_EXPOSURE_AUTO:
            mode = CAM_AE_MODE_AUTO;
            break;
        case V4L2_EXPOSURE_MANUAL:
            mode = CAM_AE_MODE_MANUAL;
            break;
        case V4L2_EXPOSURE_SHUTTER_PRIORITY:
            mode = CAM_AE_MODE_SHUTTER_PRIORITY;
            break;
        case V4L2_EXPOSURE_APERTURE_PRIORITY:
            mode = CAM_AE_MODE_APERTURE_PRIORITY;
            break;
        default:
            LOGW("Unsupported AE mode (%d), using AUTO", v4lMode);
            mode = CAM_AE_MODE_AUTO;
    }

    return mode;
}

status_t AtomISP::setEv(float bias)
{
    int evValue = (int)bias;
    LOG1("@%s: bias: %f, EV value: %d", __FUNCTION__, bias, evValue);

    status_t status = NO_ERROR;
    int ret = atomisp_set_attribute(main_fd, V4L2_CID_EXPOSURE, evValue, "exposure");
    if (ret != 0) {
        LOGE("Error setting EV in the driver");
        status = UNKNOWN_ERROR;
    }

    return status;
}

status_t AtomISP::getEv(float *bias)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int evValue = 0;

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_EXPOSURE, &evValue);
    if (ret != 0) {
        LOGE("Error getting EV from the driver");
        status = UNKNOWN_ERROR;
    }
    *bias = (float)evValue;

    return status;
}

status_t AtomISP::setAeSceneMode(SceneMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_scene_mode v4lMode;

    if (!strcmp(PlatformData::supportedSceneModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AE_SCENE_MODE_PORTRAIT:
            v4lMode = V4L2_SCENE_MODE_PORTRAIT;
            break;
        case CAM_AE_SCENE_MODE_SPORTS:
            v4lMode = V4L2_SCENE_MODE_SPORTS;
            break;
        case CAM_AE_SCENE_MODE_LANDSCAPE:
            v4lMode = V4L2_SCENE_MODE_LANDSCAPE;
            break;
        case CAM_AE_SCENE_MODE_NIGHT:
            v4lMode = V4L2_SCENE_MODE_NIGHT;
            break;
        case CAM_AE_SCENE_MODE_NIGHT_PORTRAIT:
            v4lMode = V4L2_SCENE_MODE_NIGHT;
            break;
        case CAM_AE_SCENE_MODE_FIREWORKS:
            v4lMode = V4L2_SCENE_MODE_FIREWORKS;
            break;
        case CAM_AE_SCENE_MODE_TEXT:
            v4lMode = V4L2_SCENE_MODE_TEXT;
            break;
        case CAM_AE_SCENE_MODE_SUNSET:
            v4lMode = V4L2_SCENE_MODE_SUNSET;
            break;
        case CAM_AE_SCENE_MODE_PARTY:
            v4lMode = V4L2_SCENE_MODE_PARTY_INDOOR;
            break;
        case CAM_AE_SCENE_MODE_CANDLELIGHT:
            v4lMode = V4L2_SCENE_MODE_CANDLE_LIGHT;
            break;
        case CAM_AE_SCENE_MODE_BEACH_SNOW:
            v4lMode = V4L2_SCENE_MODE_BEACH_SNOW;
            break;
        case CAM_AE_SCENE_MODE_DAWN_DUSK:
            v4lMode = V4L2_SCENE_MODE_DAWN_DUSK;
            break;
        case CAM_AE_SCENE_MODE_FALL_COLORS:
            v4lMode = V4L2_SCENE_MODE_FALL_COLORS;
            break;
        case CAM_AE_SCENE_MODE_BACKLIGHT:
            v4lMode = V4L2_SCENE_MODE_BACKLIGHT;
            break;
        default:
            LOGW("Unsupported scene mode (%d), using NONE", mode);
            v4lMode = V4L2_SCENE_MODE_NONE;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_SCENE_MODE, v4lMode, "scene mode");
    if (ret != 0) {
        LOGE("Error setting scene mode in the driver");
        status = UNKNOWN_ERROR;
    }

    return status;
}

SceneMode AtomISP::getAeSceneMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    SceneMode mode = CAM_AE_SCENE_MODE_NOT_SET;
    v4l2_scene_mode v4lMode = V4L2_SCENE_MODE_NONE;

    if (!strcmp(PlatformData::supportedSceneModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_SCENE_MODE, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting scene mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_SCENE_MODE_PORTRAIT:
            mode = CAM_AE_SCENE_MODE_PORTRAIT;
            break;
        case V4L2_SCENE_MODE_SPORTS:
            mode = CAM_AE_SCENE_MODE_SPORTS;
            break;
        case V4L2_SCENE_MODE_LANDSCAPE:
            mode = CAM_AE_SCENE_MODE_LANDSCAPE;
            break;
        case V4L2_SCENE_MODE_NIGHT:
            mode = CAM_AE_SCENE_MODE_NIGHT;
            break;
        case V4L2_SCENE_MODE_FIREWORKS:
            mode = CAM_AE_SCENE_MODE_FIREWORKS;
            break;
        case V4L2_SCENE_MODE_TEXT:
            mode = CAM_AE_SCENE_MODE_TEXT;
            break;
        case V4L2_SCENE_MODE_SUNSET:
            mode = CAM_AE_SCENE_MODE_SUNSET;
            break;
        case V4L2_SCENE_MODE_PARTY_INDOOR:
            mode = CAM_AE_SCENE_MODE_PARTY;
            break;
        case V4L2_SCENE_MODE_CANDLE_LIGHT:
            mode = CAM_AE_SCENE_MODE_CANDLELIGHT;
            break;
        case V4L2_SCENE_MODE_BEACH_SNOW:
            mode = CAM_AE_SCENE_MODE_BEACH_SNOW;
            break;
        case V4L2_SCENE_MODE_DAWN_DUSK:
            mode = CAM_AE_SCENE_MODE_DAWN_DUSK;
            break;
        case V4L2_SCENE_MODE_FALL_COLORS:
            mode = CAM_AE_SCENE_MODE_FALL_COLORS;
            break;
        case V4L2_SCENE_MODE_BACKLIGHT:
            mode = CAM_AE_SCENE_MODE_BACKLIGHT;
            break;
        default:
            LOGW("Unsupported scene mode (%d), using AUTO", v4lMode);
            mode = CAM_AE_SCENE_MODE_AUTO;
    }

    return mode;
}

status_t AtomISP::setAwbMode(AwbMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_auto_n_preset_white_balance v4lMode;

    if (!strcmp(PlatformData::supportedAwbModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AWB_MODE_AUTO:
            v4lMode = V4L2_WHITE_BALANCE_AUTO;
            break;
        case CAM_AWB_MODE_MANUAL_INPUT:
            v4lMode = V4L2_WHITE_BALANCE_MANUAL;
            break;
        case CAM_AWB_MODE_DAYLIGHT:
            v4lMode = V4L2_WHITE_BALANCE_DAYLIGHT;
            break;
        case CAM_AWB_MODE_SUNSET:
            v4lMode = V4L2_WHITE_BALANCE_INCANDESCENT;
            break;
        case CAM_AWB_MODE_CLOUDY:
            v4lMode = V4L2_WHITE_BALANCE_CLOUDY;
            break;
        case CAM_AWB_MODE_TUNGSTEN:
            v4lMode = V4L2_WHITE_BALANCE_INCANDESCENT;
            break;
        case CAM_AWB_MODE_FLUORESCENT:
            v4lMode = V4L2_WHITE_BALANCE_FLUORESCENT;
            break;
        case CAM_AWB_MODE_WARM_FLUORESCENT:
            v4lMode = V4L2_WHITE_BALANCE_FLUORESCENT_H;
            break;
        case CAM_AWB_MODE_SHADOW:
            v4lMode = V4L2_WHITE_BALANCE_SHADE;
            break;
        case CAM_AWB_MODE_WARM_INCANDESCENT:
            v4lMode = V4L2_WHITE_BALANCE_INCANDESCENT;
            break;
        default:
            LOGW("Unsupported AWB mode %d", mode);
            v4lMode = V4L2_WHITE_BALANCE_AUTO;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, v4lMode, "white balance");
    if (ret != 0) {
        LOGE("Error setting WB mode (%d) in the driver", v4lMode);
        status = UNKNOWN_ERROR;
    }

    return status;
}

AwbMode AtomISP::getAwbMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    AwbMode mode = CAM_AWB_MODE_NOT_SET;
    v4l2_auto_n_preset_white_balance v4lMode = V4L2_WHITE_BALANCE_AUTO;

    if (!strcmp(PlatformData::supportedAwbModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting WB mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_WHITE_BALANCE_AUTO:
            mode = CAM_AWB_MODE_AUTO;
            break;
        case V4L2_WHITE_BALANCE_MANUAL:
            mode = CAM_AWB_MODE_MANUAL_INPUT;
            break;
        case V4L2_WHITE_BALANCE_DAYLIGHT:
            mode = CAM_AWB_MODE_DAYLIGHT;
            break;
        case V4L2_WHITE_BALANCE_CLOUDY:
            mode = CAM_AWB_MODE_CLOUDY;
            break;
        case V4L2_WHITE_BALANCE_INCANDESCENT:
            mode = CAM_AWB_MODE_TUNGSTEN;
            break;
        case V4L2_WHITE_BALANCE_FLUORESCENT:
            mode = CAM_AWB_MODE_FLUORESCENT;
            break;
        case V4L2_WHITE_BALANCE_FLUORESCENT_H:
            mode = CAM_AWB_MODE_WARM_FLUORESCENT;
            break;
        case V4L2_WHITE_BALANCE_SHADE:
            mode = CAM_AWB_MODE_SHADOW;
            break;
        default:
            LOGW("Unsupported AWB mode %d", v4lMode);
            mode = CAM_AWB_MODE_AUTO;
    }

    return mode;
}

status_t AtomISP::setManualIso(int iso)
{
    LOG1("@%s: ISO: %d", __FUNCTION__, iso);
    status_t status = NO_ERROR;

    if (!strcmp(PlatformData::supportedIso(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return UNKNOWN_ERROR;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_ISO_SENSITIVITY, iso, "iso");
    if (ret != 0) {
        LOGE("Error setting ISO in the driver");
        status = UNKNOWN_ERROR;
    }

    return status;
}

status_t AtomISP::getManualIso(int *iso)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int isoValue = 0;

    if (!strcmp(PlatformData::supportedIso(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_ISO_SENSITIVITY, &isoValue);
    if (ret != 0) {
        LOGE("Error getting ISO from the driver");
        status = UNKNOWN_ERROR;
    }
    *iso = isoValue;

    return status;
}

status_t AtomISP::setIsoMode(IsoMode mode) {
    /*ISO mode not supported for SOC sensor yet.*/
    LOG1("@%s", __FUNCTION__);
    status_t status = INVALID_OPERATION;
    return status;
}

IsoMode AtomISP::getIsoMode(void) {
    /*ISO mode not supported for SOC sensor yet.*/
    return CAM_AE_ISO_MODE_NOT_SET;
}

status_t AtomISP::setAeMeteringMode(MeteringMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_exposure_metering v4lMode;

    if (!strcmp(PlatformData::supportedAeMetering(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AE_METERING_MODE_AUTO:
            v4lMode = V4L2_EXPOSURE_METERING_AVERAGE;
            break;
        case CAM_AE_METERING_MODE_SPOT:
            v4lMode = V4L2_EXPOSURE_METERING_SPOT;
            break;
        case CAM_AE_METERING_MODE_CENTER:
            v4lMode = V4L2_EXPOSURE_METERING_CENTER_WEIGHTED;
            break;
        default:
            LOGW("Unsupported AE metering mode (%d), using AVERAGE", mode);
            v4lMode = V4L2_EXPOSURE_METERING_AVERAGE;
            break;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_EXPOSURE_METERING, v4lMode, "AE metering mode");
    if (ret != 0) {
        LOGE("Error setting AE metering mode (%d) in the driver", v4lMode);
        status = UNKNOWN_ERROR;
    }

    return status;
}

MeteringMode AtomISP::getAeMeteringMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    MeteringMode mode = CAM_AE_METERING_MODE_NOT_SET;
    v4l2_exposure_metering v4lMode = V4L2_EXPOSURE_METERING_AVERAGE;

    if (!strcmp(PlatformData::supportedAeMetering(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_EXPOSURE_METERING, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting AE metering mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_EXPOSURE_METERING_CENTER_WEIGHTED:
            mode = CAM_AE_METERING_MODE_CENTER;
            break;
        case V4L2_EXPOSURE_METERING_SPOT:
            mode = CAM_AE_METERING_MODE_SPOT;
            break;
        case V4L2_EXPOSURE_METERING_AVERAGE:
            mode = CAM_AE_METERING_MODE_AUTO;
            break;
        default:
            LOGW("Unsupported AE metering mode (%d), using AUTO", v4lMode);
            mode = CAM_AE_METERING_MODE_AUTO;
            break;
    }

    return mode;
}

status_t AtomISP::set3AColorEffect(const char *effect)
{
    LOG1("@%s: effect = %s", __FUNCTION__, effect);
    status_t status = NO_ERROR;

    v4l2_colorfx v4l2Effect = V4L2_COLORFX_NONE;
    if (strncmp(effect, CameraParameters::EFFECT_MONO, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_BW;
    else if (strncmp(effect, CameraParameters::EFFECT_NEGATIVE, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_NEGATIVE;
    else if (strncmp(effect, CameraParameters::EFFECT_SEPIA, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_SEPIA;
    else if (strncmp(effect, IntelCameraParameters::EFFECT_STILL_SKY_BLUE, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_SKY_BLUE;
    else if (strncmp(effect, IntelCameraParameters::EFFECT_STILL_GRASS_GREEN, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_GRASS_GREEN;
    else if (strncmp(effect, IntelCameraParameters::EFFECT_STILL_SKIN_WHITEN_MEDIUM, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_SKIN_WHITEN;
    else if (strncmp(effect, IntelCameraParameters::EFFECT_VIVID, sizeof(effect)) == 0)
        v4l2Effect = V4L2_COLORFX_VIVID;

    // following two values need a explicit cast as the
    // definitions in hardware/intel/linux-2.6/include/linux/atomisp.h
    // have incorrect type (properly defined values are in videodev2.h)
    else if (effect == IntelCameraParameters::EFFECT_STILL_SKIN_WHITEN_LOW)
        v4l2Effect = (v4l2_colorfx)V4L2_COLORFX_SKIN_WHITEN_LOW;
    else if (effect == IntelCameraParameters::EFFECT_STILL_SKIN_WHITEN_HIGH)
        v4l2Effect = (v4l2_colorfx)V4L2_COLORFX_SKIN_WHITEN_HIGH;
    else if (strncmp(effect, CameraParameters::EFFECT_NONE, strlen(effect)) != 0){
        LOGE("Color effect not found.");
        status = -1;
        // Fall back to the effect NONE
    }

    status = setColorEffect(v4l2Effect);
    status = applyColorEffect();
    return status;
}

void AtomISP::getDefaultParams(CameraParameters *params, CameraParameters *intel_params)
{
    LOG1("@%s", __FUNCTION__);
    if (!params) {
        LOGE("params is null!");
        return;
    }

    // multipoint focus
    params->set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS, 0);
    // set empty area
    params->set(CameraParameters::KEY_FOCUS_AREAS, "(0,0,0,0,0)");

    // metering areas
    params->set(CameraParameters::KEY_MAX_NUM_METERING_AREAS, 0);
    // set empty area
    params->set(CameraParameters::KEY_METERING_AREAS, "(0,0,0,0,0)");

    // TODO: Add here any V4L2 3A specific settings
}

status_t AtomISP::setAfMode(AfMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_auto_focus_range v4lMode;

    // TODO: add supported modes to PlatformData
    if (getCurrentCameraId() > 0) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AF_MODE_AUTO:
            v4lMode = V4L2_AUTO_FOCUS_RANGE_AUTO;
            break;
        case CAM_AF_MODE_MACRO:
            v4lMode = V4L2_AUTO_FOCUS_RANGE_MACRO;
            break;
        case CAM_AF_MODE_INFINITY:
            v4lMode = V4L2_AUTO_FOCUS_RANGE_INFINITY;
            break;
        default:
            LOGW("Unsupported AF mode (%d), using AUTO", mode);
            v4lMode = V4L2_AUTO_FOCUS_RANGE_AUTO;
            break;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_AUTO_FOCUS_RANGE , v4lMode, "AF mode");
    if (ret != 0) {
        LOGE("Error setting AF  mode (%d) in the driver", v4lMode);
        status = UNKNOWN_ERROR;
    }

    return status;
}

AfMode AtomISP::getAfMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    AfMode mode = CAM_AF_MODE_AUTO;
    v4l2_auto_focus_range v4lMode = V4L2_AUTO_FOCUS_RANGE_AUTO;

    // TODO: add supported modes to PlatformData
    if (getCurrentCameraId() > 0) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_AUTO_FOCUS_RANGE, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting AF mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_AUTO_FOCUS_RANGE_AUTO:
            mode = CAM_AF_MODE_AUTO;
            break;
        case CAM_AF_MODE_MACRO:
            mode = CAM_AF_MODE_MACRO;
            break;
        case CAM_AF_MODE_INFINITY:
            mode = CAM_AF_MODE_INFINITY;
            break;
        default:
            LOGW("Unsupported AF mode (%d), using AUTO", v4lMode);
            mode = CAM_AF_MODE_AUTO;
            break;
    }

    return mode;
}

status_t AtomISP::getGridWindow(AAAWindowInfo& window)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    atomisp_parm isp_param;
    if (getIspParameters(&isp_param) < 0)
        return UNKNOWN_ERROR;

    window.width = isp_param.info.s3a_width * isp_param.info.s3a_bqs_per_grid_cell * 2;
    window.height = isp_param.info.s3a_height * isp_param.info.s3a_bqs_per_grid_cell * 2;

    return status;
}

status_t AtomISP::setAfEnabled(bool enable)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    // TODO: add supported modes to PlatformData
    if (getCurrentCameraId() > 0) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_FOCUS_AUTO, enable, "Auto Focus");
    if (ret != 0) {
        LOGE("Error setting Auto Focus (%d) in the driver", enable);
        status = UNKNOWN_ERROR;
    }

    return status;
}

int AtomISP::get3ALock()
{
    LOG1("@%s", __FUNCTION__);
    int aaaLock = 0;

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_3A_LOCK, &aaaLock);
    if (ret != 0) {
        LOGE("Error getting 3A Lock setting from the driver");
    }

    return aaaLock;
}

bool AtomISP::getAeLock()
{
    LOG1("@%s", __FUNCTION__);
    bool aeLockEnabled = false;

    int aaaLock = get3ALock();
    if (aaaLock & V4L2_LOCK_EXPOSURE)
        aeLockEnabled = true;

    return aeLockEnabled;
}

status_t AtomISP::setAeLock(bool enable)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    int aaaLock = get3ALock();
    if (enable)
        aaaLock |= V4L2_LOCK_EXPOSURE;
    else
        aaaLock &= ~V4L2_LOCK_EXPOSURE;

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_3A_LOCK, aaaLock, "AE Lock");
    if (ret != 0) {
        LOGE("Error setting AE lock (%d) in the driver", enable);
        status = UNKNOWN_ERROR;
    }

    return status;
}

bool AtomISP::getAfLock()
{
    LOG1("@%s", __FUNCTION__);
    bool afLockEnabled = false;

    int aaaLock = get3ALock();
    if (aaaLock & V4L2_LOCK_FOCUS)
        afLockEnabled = true;

    return afLockEnabled;
}

status_t AtomISP::setAfLock(bool enable)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    int aaaLock = get3ALock();
    if (enable)
        aaaLock |= V4L2_LOCK_FOCUS;
    else
        aaaLock &= ~V4L2_LOCK_FOCUS;

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_3A_LOCK, aaaLock, "AF Lock");
    if (ret != 0) {
        LOGE("Error setting AF lock (%d) in the driver", enable);
        status = UNKNOWN_ERROR;
    }

    return status;
}

bool AtomISP::getAwbLock()
{
    LOG1("@%s", __FUNCTION__);
    bool awbLockEnabled = false;

    int aaaLock = get3ALock();
    if (aaaLock & V4L2_LOCK_WHITE_BALANCE)
        awbLockEnabled = true;

    return awbLockEnabled;
}

status_t AtomISP::setAwbLock(bool enable)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    int aaaLock = get3ALock();
    if (enable)
        aaaLock |= V4L2_LOCK_WHITE_BALANCE;
    else
        aaaLock &= ~V4L2_LOCK_WHITE_BALANCE;

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_3A_LOCK, aaaLock, "AF Lock");
    if (ret != 0) {
        LOGE("Error setting AWB lock (%d) in the driver", enable);
        status = UNKNOWN_ERROR;
    }

    return status;
}

status_t AtomISP::getCurrentFocusPosition(int *pos)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int position = 0;

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_FOCUS_ABSOLUTE , &position);
    if (ret != 0) {
        LOGE("Error getting Focus Position from the driver");
        status = UNKNOWN_ERROR;
    }
    *pos = position;

    return status;
}

status_t AtomISP::applyEv(float bias)
{
    LOG1("@%s: bias: %f", __FUNCTION__, bias);
    return setEv(bias);
}

status_t AtomISP::setManualShutter(float expTime)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    int time = expTime / 0.0001; // 100 usec units

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_EXPOSURE_ABSOLUTE, time, "Exposure time");
    if (ret != 0) {
        LOGE("Error setting Exposure time (%d) in the driver", time);
        status = UNKNOWN_ERROR;
    }

    return status;
}

status_t AtomISP::setAeFlashMode(FlashMode mode)
{
    LOG1("@%s: %d", __FUNCTION__, mode);
    status_t status = NO_ERROR;
    v4l2_flash_led_mode v4lMode;

    if (!strcmp(PlatformData::supportedFlashModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return INVALID_OPERATION;
    }

    switch (mode)
    {
        case CAM_AE_FLASH_MODE_OFF:
            v4lMode = V4L2_FLASH_LED_MODE_NONE;
            break;
        case CAM_AE_FLASH_MODE_ON:
            v4lMode = V4L2_FLASH_LED_MODE_FLASH;
            break;
        case CAM_AE_FLASH_MODE_TORCH:
            v4lMode = V4L2_FLASH_LED_MODE_TORCH;
            break;
        default:
            LOGW("Unsupported Flash mode (%d), using OFF", mode);
            v4lMode = V4L2_FLASH_LED_MODE_NONE;
            break;
    }

    int ret = atomisp_set_attribute(main_fd, V4L2_CID_FLASH_LED_MODE , v4lMode, "Flash mode");
    if (ret != 0) {
        LOGE("Error setting Flash mode (%d) in the driver", v4lMode);
        status = UNKNOWN_ERROR;
    }

    return status;
}

FlashMode AtomISP::getAeFlashMode()
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    FlashMode mode = CAM_AE_FLASH_MODE_OFF;
    v4l2_flash_led_mode v4lMode = V4L2_FLASH_LED_MODE_NONE;

    if (!strcmp(PlatformData::supportedFlashModes(getCurrentCameraId()), "")) {
        LOG1("@%s: not supported by current camera", __FUNCTION__);
        return mode;
    }

    int ret = atomisp_get_attribute(main_fd, V4L2_CID_FLASH_LED_MODE, (int*) &v4lMode);
    if (ret != 0) {
        LOGE("Error getting Flash mode from the driver");
        status = UNKNOWN_ERROR;
    }

    switch (v4lMode)
    {
        case V4L2_FLASH_LED_MODE_NONE:
            mode = CAM_AE_FLASH_MODE_OFF;
            break;
        case V4L2_FLASH_LED_MODE_FLASH:
            mode = CAM_AE_FLASH_MODE_ON;
            break;
        case V4L2_FLASH_LED_MODE_TORCH:
            mode = CAM_AE_FLASH_MODE_TORCH;
            break;
        default:
            LOGW("Unsupported Flash mode (%d), using OFF", v4lMode);
            mode = CAM_AE_FLASH_MODE_OFF;
            break;
    }

    return mode;
}

void AtomISP::setPublicAeMode(AeMode mode)
{
    LOG2("@%s", __FUNCTION__);
    mPublicAeMode = mode;
}

AeMode AtomISP::getPublicAeMode()
{
    LOG2("@%s", __FUNCTION__);
    return mPublicAeMode;
}

void AtomISP::setPublicAfMode(AfMode mode)
{
    LOG2("@%s", __FUNCTION__);
    mPublicAfMode = mode;
}

AfMode AtomISP::getPublicAfMode()
{
    LOG2("@%s", __FUNCTION__);
    return mPublicAfMode;
}

} // namespace android

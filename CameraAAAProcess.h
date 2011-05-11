/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_HARDWARE_CAMERA_AAA_PROCESS_H
#define ANDROID_HARDWARE_CAMERA_AAA_PROCESS_H

#include <utils/threads.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ci_adv_pub.h"
#include "ci_adv_property.h"
#include "v4l2.h"

#ifdef __cplusplus
}
#endif

namespace android {


typedef enum ENUM_SENSOR_TYPE {
    ENUM_SENSOR_TYPE_SOC = 0,
    ENUM_SENSOR_TYPE_RAW = 1
} ENUM_SENSOR_TYPE;

typedef enum
{
    CAM_AWB_MODE_AUTO,
    CAM_AWB_MODE_DAYLIGHT,
    CAM_AWB_MODE_SUNSET,
    CAM_AWB_MODE_CLOUDY,
    CAM_AWB_MODE_TUNGSTEN,
    CAM_AWB_MODE_FLUORESCENT,
    CAM_AWB_MODE_WARM_FLUORESCENT,
    CAM_AWB_MODE_SHADOW,
    CAM_AWB_MODE_WARM_INCANDESCENT
} cam_awb_mode_t;

typedef enum
{
    CAM_FOCUS_MODE_AUTO,
    CAM_FOCUS_MODE_MACRO,
    CAM_FOCUS_MODE_FULL,
    CAM_FOCUS_MODE_NORM
} cam_focus_mode_t;

typedef enum {
    CAM_AEFLICKER_MODE_OFF,
    CAM_AEFLICKER_MODE_50HZ,
    CAM_AEFLICKER_MODE_60HZ,
    CAM_AEFLICKER_MODE_AUTO
} cam_aeflicker_mode_t;

typedef enum
{
    CAM_FLASH_MODE_AUTO,
    CAM_FLASH_MODE_OFF,
    CAM_FLASH_MODE_ON,
    CAM_FLASH_MODE_RED_EYE,
    CAM_FLASH_MODE_TORCH,
} cam_ae_flash_mode_t;

typedef enum
{
    CAM_SCENE_MODE_AUTO,
    CAM_SCENE_MODE_PORTRAIT,
    CAM_SCENE_MODE_SPORTS,
    CAM_SCENE_MODE_LANDSCAPE,
    CAM_SCENE_MODE_NIGHT,
    CAM_SCENE_MODE_FIREWORKS
} cam_ae_scene_mode_t;

typedef struct {
    int x_left;
    int x_right;
    int y_top;
    int y_bottom;
    int     weight;
} cam_Window;


#define AAA_FAIL    1
#define AAA_SUCCESS 0

class AAAProcess {
public:
    AAAProcess(unsigned int sensortype);
    ~AAAProcess();

    void Init(void);
    void Uninit(void);

    void IspSetFd(int fd);

    void AeProcess(void);
    void AfProcess(void);
    void AwbProcess(void);

    void GetStatistics(void);

    void AeApplyResults(void);
    void AwbApplyResults(void);

    int ModeSpecInit(void);    /* Called when switch the resolution */
    void SwitchMode(CI_ISP_MODE mode);

    void AfStillStart(void);
    void AfStillStop(void);
    int AfStillIsComplete(void);
    int AeCalcForFlash(void);
    int AeCalcWithoutFlash(void);
    int AeCalcWithFlash(void);
    int AwbCalcFlash(void);

    void DisReadStatistics(void);
    void DisProcess(ci_adv_dis_vector *dis_vector);
    void DisCalcStill(ci_adv_dis_vector *vector, int frame_number);
    void UpdateDisResults(void);
    void StillCompose(struct user_buffer *com_buf,
                      struct user_buffer bufs[], int frame_dis, ci_adv_dis_vector vectors[]);

    void DoRedeyeRemoval(struct user_buffer *user_buf); /* TBD */

    void LoadGdcTable(void);

    int AeSetMode(ci_adv_AeMode mode);
    int AeGetMode(ci_adv_AeMode *mode);
    int AeSetMeteringMode(ci_adv_AeMeteringMode mode);
    int AeGetMeteringMode(ci_adv_AeMeteringMode *mode);

    int AeSetEv(int bias);
    int AeGetEv(int *bias);

    int AeSetSceneMode(int mode);
    int AeGetSceneMode(int *mode);
    int AeSetFlashMode(int mode);
    int AeGetFlashMode(int *mode);
    int AeIsFlashNecessary(bool *used);

    int AeSetFlickerMode(cam_aeflicker_mode_t mode);
    int AeGetFlickerMode(cam_aeflicker_mode_t *mode);

    int AeSetManualIso(int sensitivity);
    int AeGetManualIso(int *sensitivity);


    int AeSetWindow(const cam_Window *window);
    int AeGetWindow(cam_Window *window);

    int AwbSetMode (int wb_mode);
    int AwbGetMode(int *wb_mode);

    int AfSetMode(int mode);
    int AfGetMode(int *mode);
    int AfSetMeteringMode(ci_adv_AfMeteringMode mode);
    int AfGetMeteringMode(ci_adv_AfMeteringMode *mode);
    int AfSetWindow(const cam_Window *window);
    int AfGetWindow(cam_Window *window);

    void SetAfEnabled(unsigned int enabled) {
        mAfEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetAfStillEnabled(unsigned int enabled) {
        mAfStillEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetAeEnabled(unsigned int enabled) {
        mAeEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetAwbEnabled(unsigned int enabled) {
        mAwbEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetStillStabilizationEnabled(unsigned int enabled) {
        mStillStabilizationEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetGdcEnabled(unsigned int enabled) {
        mGdcEnabled = ((enabled > 0) ? 1: 0);
    }
    void SetRedEyeRemovalEnabled(unsigned int enabled) {
        mRedEyeRemovalEnabled = ((enabled > 0) ? 1: 0);
    }

    unsigned int GetAfEnabled(void) {
        return mAfEnabled ;
    }
    unsigned int GetAfStillEnabled(void) {
        return mAfStillEnabled ;
    }
    unsigned int GetAeEnabled(void) {
        return mAeEnabled;
    }
    unsigned int GetAwbEnabled(void) {
        return mAwbEnabled;
    }
    unsigned int GetStillStabilizationEnabled(void) {
        return mStillStabilizationEnabled;
    }
    unsigned int GetGdcEnabled(void) {
        return mGdcEnabled;
    }
    unsigned int GetRedEyeRemovalEnabled(void) {
        return mRedEyeRemovalEnabled;
    }

    unsigned int GetAfStillFrames() {
        return mAfStillFrames;
    }

    void SetAfStillFrames(unsigned int frames) {
        mAfStillFrames = frames;
    }

#define AF_STILL_MAX_FRAMES 100
    unsigned int GetAfStillIsOverFrames() {
        return (mAfStillFrames >= AF_STILL_MAX_FRAMES);
    }

private:
    /* not 0 is enabled, 0 is disabled */
    unsigned int mAeEnabled;
    unsigned int mAfEnabled;    // for preview
    unsigned int mAfStillEnabled; // still af
    unsigned int mAwbEnabled;
    unsigned int mRedEyeRemovalEnabled;
    unsigned int mStillStabilizationEnabled;
    unsigned int mGdcEnabled;

    unsigned int mAwbMode;
    unsigned int mAfMode;

    unsigned int mSensorType;

    //static const unsigned int mAfStillMaxFrames = 500;
    unsigned int  mAfStillFrames;  // 100 frames will time out

    unsigned int mInitied;    // 0 means not init, not 0 means has been initied.
};


}; // namespace android

#endif // ANDROID_HARDWARE_CAMERA_AAA_PROCESS_H

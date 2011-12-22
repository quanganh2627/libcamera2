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

#define LOG_TAG "AAAProcess"
#include <math.h>
#include "CameraAAAProcess.h"
#include "LogHelper.h"


namespace android {


AAAProcess::AAAProcess(void)
: mGdcEnabled(false),
  mAwbMode(CAM_AWB_MODE_AUTO),
  mAfMode(CAM_AF_MODE_AUTO),
  mSensorType(~0),
  mAfStillFrames(0),
  mDoneStatistics(false),
  mInitied(false)
{
    mSensorType = SENSOR_TYPE_SOC;
    mAeMode = CAM_AE_MODE_AUTO;
    mFocusPosition = 50;
    mColorTemperature = 5000;
    mManualAperture = 2.8;
    mManualShutter = 1 /60.0;
    mManualIso = 100;
    dvs_vector.x = 0;
    dvs_vector.y = 0;
}

AAAProcess::~AAAProcess()
{
}

int AAAProcess::AeLock(bool lock)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(SENSOR_TYPE_RAW == mSensorType)
        return ci_adv_ae_lock(lock);
    else
        return 0;
}

int AAAProcess::AeIsLocked(bool *lock)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(SENSOR_TYPE_RAW == mSensorType)
        return ci_adv_ae_is_locked(lock);
    else
        return 0;
}

void AAAProcess::SetAfEnabled(bool enabled)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(SENSOR_TYPE_RAW == mSensorType)
        ci_adv_af_enable(enabled);
}

void AAAProcess::SetAeEnabled(bool enabled)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(SENSOR_TYPE_RAW == mSensorType)
        ci_adv_ae_enable(enabled);
}

void AAAProcess::SetAwbEnabled(bool enabled)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(SENSOR_TYPE_RAW == mSensorType)
        ci_adv_awb_enable(enabled);
}

void AAAProcess::SwitchMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_isp_mode isp_mode;
        switch (mode) {
        case PREVIEW_MODE:
            isp_mode = ci_adv_isp_mode_preview;
            break;
        case STILL_IMAGE_MODE:
            isp_mode = ci_adv_isp_mode_capture;
            break;
        case VIDEO_RECORDING_MODE:
            isp_mode = ci_adv_isp_mode_video;
            break;
        default:
            isp_mode = ci_adv_isp_mode_preview;
            LogWarning("Wrong sensor mode %d", mode);
            break;
        }
        ci_adv_switch_mode(isp_mode);
    }
}

void AAAProcess::SetFrameRate(float framerate)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_set_frame_rate(CI_ADV_S15_16_FROM_FLOAT(framerate));
    }
}

int AAAProcess::AeAfAwbProcess(bool read_stats)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return 0;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        if (ci_adv_process_frame(read_stats) < 0)
            return -1;
        mDoneStatistics = true;
    }

    return 0;
}

void AAAProcess::DvsProcess(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        if (true != mDoneStatistics) {
            LogDetail("dvs,line:%d in DvsProcess, mDoneStatistics is false", __LINE__);
            return;
        }
        ci_adv_dvs_process();
    }
}

void AAAProcess::AfStillStart(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_af_start();
    }
}

void AAAProcess::AfStillStop(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_af_stop();
    }
}

int AAAProcess::AfStillIsComplete(bool *complete)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *complete = ci_adv_af_is_complete();
    }

    return AAA_SUCCESS;
}

int AAAProcess::PreFlashProcess(cam_flash_stage stage)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_flash_stage wr_stage;
        switch (stage)
        {
        case CAM_FLASH_STAGE_NONE:
            wr_stage = ci_adv_flash_stage_none;
            break;
        case CAM_FLASH_STAGE_PRE:
            wr_stage = ci_adv_flash_stage_pre;
            break;
        case CAM_FLASH_STAGE_MAIN:
            wr_stage = ci_adv_flash_stage_main;
            break;
        default:
            LogError("Flash stage not defined!");
            return AAA_FAIL;

        }
        ci_adv_process_for_flash (wr_stage);
    }

    return AAA_SUCCESS;

}

void AAAProcess::SetStillStabilizationEnabled(bool en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_dis_enable(en);
    }
}

void AAAProcess::GetStillStabilizationEnabled(bool *en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *en = ci_adv_dis_is_enabled();
    }
}

void AAAProcess::DisCalcStill(ci_adv_dis_vector *vector, int frame_number)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_dis_calc_still(vector, frame_number);
    }
}

void AAAProcess::StillCompose(ci_adv_user_buffer *com_buf,
        ci_adv_user_buffer bufs[], int frame_dis,
        ci_adv_dis_vector vectors[])
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_still_compose(com_buf, bufs, frame_dis, vectors);
    }
}

void AAAProcess::DoRedeyeRemoval(void *img_buf, int size, int width, int height, int format)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_user_buffer user_buf;
        switch (format)
        {
        case V4L2_PIX_FMT_YUV420:
            user_buf.format= ci_adv_frame_format_yuv420;
            break;
        default:
            LogError("Unsupported format in red eye removal!");
            return;
        }
        user_buf.addr = img_buf;
        user_buf.width = width;
        user_buf.height = height;
        user_buf.length = size;
        ci_adv_correct_redeyes(&user_buf);
    }
}

void AAAProcess::LoadGdcTable(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if(!mInitied)
        return;

    if(!mGdcEnabled)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_load_gdc_table();
    }
}

int AAAProcess::AeSetMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_mode wr_val;
        switch (mode)
        {
        case CAM_AE_MODE_AUTO:
            wr_val = ci_adv_ae_mode_auto;
            break;
        case CAM_AE_MODE_MANUAL:
            wr_val = ci_adv_ae_mode_manual;
            break;
        case CAM_AE_MODE_SHUTTER_PRIORITY:
            wr_val = ci_adv_ae_mode_shutter_priority;
            break;
        case CAM_AE_MODE_APERTURE_PRIORITY:
            wr_val = ci_adv_ae_mode_aperture_priority;
            break;
        default:
            LogError("Invalid set AE mode!");
            wr_val = ci_adv_ae_mode_auto;
        }
        ci_adv_err ret = ci_adv_ae_set_mode(wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        mAeMode = mode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_mode rd_val;
        ci_adv_err ret = ci_adv_ae_get_mode(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_ae_mode_auto:
            *mode = CAM_AE_MODE_AUTO;
            break;
        case ci_adv_ae_mode_manual:
            *mode = CAM_AE_MODE_MANUAL;
            break;
        case ci_adv_ae_mode_shutter_priority:
            *mode = CAM_AE_MODE_SHUTTER_PRIORITY;
            break;
        case ci_adv_ae_mode_aperture_priority:
            *mode = CAM_AE_MODE_APERTURE_PRIORITY;
            break;
        default:
            LogError("Invalid get AE mode!");
            *mode = CAM_AE_MODE_AUTO;
        }
        mAeMode = *mode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetMeteringMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_metering_mode wr_val;
        switch (mode)
        {
        case CAM_AE_METERING_MODE_SPOT:
            wr_val = ci_adv_ae_metering_mode_spot;
            break;
        case CAM_AE_METERING_MODE_CENTER:
            wr_val = ci_adv_ae_metering_mode_center;
            break;
        case CAM_AE_METERING_MODE_CUSTOMIZED:
            wr_val = ci_adv_ae_metering_mode_customized;
            break;
        case CAM_AE_METERING_MODE_AUTO:
            wr_val = ci_adv_ae_metering_mode_auto;
            break;
        default:
            LogError("Invalid set AE metering mode!");
            wr_val = ci_adv_ae_metering_mode_auto;
        }
        ci_adv_err ret = ci_adv_ae_set_metering_mode(wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetMeteringMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_metering_mode rd_val;
        ci_adv_err ret = ci_adv_ae_get_metering_mode(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_ae_metering_mode_spot:
            *mode = CAM_AE_METERING_MODE_SPOT;
            break;
        case ci_adv_ae_metering_mode_center:
            *mode = CAM_AE_METERING_MODE_CENTER;
            break;
        case ci_adv_ae_metering_mode_customized:
            *mode = CAM_AE_METERING_MODE_CUSTOMIZED;
            break;
        case ci_adv_ae_metering_mode_auto:
            *mode = CAM_AE_METERING_MODE_AUTO;
            break;
        default:
            LogError("Invalid get AE metering mode!");
            *mode = CAM_AE_METERING_MODE_AUTO;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetEv(float bias)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        bias = bias > 2 ? 2 : bias;
        bias = bias < -2 ? -2 : bias;
        ci_adv_err ret = ci_adv_ae_set_bias(CI_ADV_S15_16_FROM_FLOAT(bias));
        if(ci_adv_success != ret)
        {
            LogError("!!!line:%d, in AeSetEv, ret:%d", __LINE__, ret);
            return AAA_FAIL;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetEv(float *bias)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        int ibias;
        ci_adv_err ret = ci_adv_ae_get_bias(&ibias);
        *bias = CI_ADV_S15_16_TO_FLOAT(ibias);
        if(ci_adv_success != ret)
        {
            LogError("!!!line:%d, in AeGetEv, ret:%d", __LINE__, ret);
            return AAA_FAIL;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetSceneMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_exposure_program wr_val;
        switch (mode) {
        case CAM_AE_SCENE_MODE_AUTO:
            wr_val = ci_adv_ae_exposure_program_auto;
            break;
        case CAM_AE_SCENE_MODE_PORTRAIT:
            wr_val = ci_adv_ae_exposure_program_portrait;
            break;
        case CAM_AE_SCENE_MODE_SPORTS:
            wr_val = ci_adv_ae_exposure_program_sports;
            break;
        case CAM_AE_SCENE_MODE_LANDSCAPE:
            wr_val = ci_adv_ae_exposure_program_landscape;
            break;
        case CAM_AE_SCENE_MODE_NIGHT:
            wr_val = ci_adv_ae_exposure_program_night;
            break;
        case CAM_AE_SCENE_MODE_FIREWORKS:
            wr_val = ci_adv_ae_exposure_program_fireworks;
            break;
        default:
            LogError("Invalid set AE scene mode!");
            wr_val = ci_adv_ae_exposure_program_auto;
        }
        ci_adv_err ret = ci_adv_ae_set_exposure_program (wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetSceneMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_exposure_program rd_val;
        ci_adv_err ret = ci_adv_ae_get_exposure_program (&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val) {
        case ci_adv_ae_exposure_program_auto:
            *mode = CAM_AE_SCENE_MODE_AUTO;
            break;
        case ci_adv_ae_exposure_program_portrait:
            *mode = CAM_AE_SCENE_MODE_PORTRAIT;
            break;
        case ci_adv_ae_exposure_program_sports:
            *mode = CAM_AE_SCENE_MODE_SPORTS;
            break;
        case ci_adv_ae_exposure_program_landscape:
            *mode = CAM_AE_SCENE_MODE_LANDSCAPE;
            break;
        case ci_adv_ae_exposure_program_night:
            *mode = CAM_AE_SCENE_MODE_NIGHT;
            break;
        case ci_adv_ae_exposure_program_fireworks:
            *mode = CAM_AE_SCENE_MODE_FIREWORKS;
            break;
        default:
            LogError("Invalid get AE scene mode!");
            *mode = CAM_AE_SCENE_MODE_AUTO;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetFlashMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_flash_mode wr_val;
        switch (mode) {
        case CAM_AE_FLASH_MODE_AUTO:
            wr_val = ci_adv_ae_flash_mode_auto;
            break;
        case CAM_AE_FLASH_MODE_OFF:
            wr_val = ci_adv_ae_flash_mode_off;
            break;
        case CAM_AE_FLASH_MODE_ON:
            wr_val = ci_adv_ae_flash_mode_on;
            break;
        case CAM_AE_FLASH_MODE_DAY_SYNC:
            wr_val = ci_adv_ae_flash_mode_day_sync;
            break;
        case CAM_AE_FLASH_MODE_SLOW_SYNC:
            wr_val = ci_adv_ae_flash_mode_slow_sync;
            break;
        default:
            LogError("Invalid set flash mode!");
            wr_val = ci_adv_ae_flash_mode_auto;
        }
        ci_adv_err ret = ci_adv_ae_set_flash_mode(wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetFlashMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_flash_mode rd_val;
        ci_adv_err ret = ci_adv_ae_get_flash_mode(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val) {
        case ci_adv_ae_flash_mode_auto:
            *mode = CAM_AE_FLASH_MODE_AUTO;
            break;
        case ci_adv_ae_flash_mode_off:
            *mode = CAM_AE_FLASH_MODE_OFF;
            break;
        case ci_adv_ae_flash_mode_on:
            *mode = CAM_AE_FLASH_MODE_ON;
            break;
        case ci_adv_ae_flash_mode_day_sync:
            *mode = CAM_AE_FLASH_MODE_DAY_SYNC;
            break;
        case ci_adv_ae_flash_mode_slow_sync:
            *mode = CAM_AE_FLASH_MODE_SLOW_SYNC;
            break;
        default:
            LogError("Invalid get flash mode!");
            *mode = CAM_AE_FLASH_MODE_AUTO;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeIsFlashNecessary(bool *used)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    *used = false;
    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_is_flash_necessary(used);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetFlickerMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_flicker_mode wr_val;
        switch (mode)
        {
        case CAM_AE_FLICKER_MODE_OFF:
            wr_val = ci_adv_ae_flicker_mode_off;
            break;
        case CAM_AE_FLICKER_MODE_50HZ:
            wr_val = ci_adv_ae_flicker_mode_50hz;
            break;
        case CAM_AE_FLICKER_MODE_60HZ:
            wr_val = ci_adv_ae_flicker_mode_60hz;
            break;
        case CAM_AE_FLICKER_MODE_AUTO:
            wr_val = ci_adv_ae_flicker_mode_auto;
            break;
        default:
            LogError("Invalid set flicker mode!");
            wr_val = ci_adv_ae_flicker_mode_auto;
        }
        ci_adv_err ret = ci_adv_ae_set_flicker_mode(wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetFlickerMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_flicker_mode rd_val;
        ci_adv_err ret = ci_adv_ae_get_flicker_mode(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_ae_flicker_mode_off:
            *mode = CAM_AE_FLICKER_MODE_OFF;
            break;
        case ci_adv_ae_flicker_mode_50hz:
            *mode = CAM_AE_FLICKER_MODE_50HZ;
            break;
        case ci_adv_ae_flicker_mode_60hz:
            *mode = CAM_AE_FLICKER_MODE_60HZ;
            break;
        case ci_adv_ae_flicker_mode_auto:
            *mode = CAM_AE_FLICKER_MODE_AUTO;
            break;
        default:
            LogError("Invalid get flicker mode!");
            *mode = CAM_AE_FLICKER_MODE_AUTO;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetManualIso(int sensitivity, bool to_hw)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        float fev;
        if (sensitivity <=0)
        {
            LogError("error in get log2 math computation in line %d", __LINE__);
            return AAA_FAIL;
        }

        mManualIso = sensitivity;

        if(to_hw)
        {
            fev = log10((float)sensitivity / 3.125) / log10(2.0);
            ci_adv_err ret = ci_adv_ae_set_manual_iso(CI_ADV_S15_16_FROM_FLOAT(fev));
            if(ci_adv_success != ret)
                return AAA_FAIL;

            LOGD(" *** manual set iso in EV: %f\n", fev);
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetManualIso(int *sensitivity)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        int iev;
        ci_adv_err ret = ci_adv_ae_get_manual_iso(&iev);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        *sensitivity = (int)(3.125 * pow(2, CI_ADV_S15_16_TO_FLOAT(iev)));
        mManualIso = *sensitivity;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetManualAperture(float aperture, bool to_hw)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        float fev;
        if (aperture <=0)
        {
            LogError("error in get log2 math computation in line %d\n", __LINE__);
            return AAA_FAIL;
        }

        mManualAperture = aperture;

        if (to_hw)
        {
            fev = 2.0 * (log10(aperture) / log10(2.0));
            ci_adv_err ret = ci_adv_ae_set_manual_aperture(CI_ADV_S15_16_FROM_FLOAT(fev));
            if(ci_adv_success != ret)
                return AAA_FAIL;

            LOGD(" *** manual set aperture in EV: %f\n", fev);
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetManualAperture(float *aperture)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        int iev;
        ci_adv_err ret = ci_adv_ae_get_manual_aperture(&iev);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        *aperture = pow(2, CI_ADV_S15_16_TO_FLOAT(iev) / 2.0);
        mManualAperture = *aperture;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetManualBrightness(float *brightness)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        int val;
        ci_adv_err ret = ci_adv_ae_get_manual_brightness(&val);
        if (ci_adv_success != ret)
            return AAA_FAIL;

        *brightness = (float)((float)val / 65536.0);
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetManualShutter(float exp_time, bool to_hw)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        float fev;
        if (exp_time <=0)
        {
            LogError("error in get log2 math computation in line %d", __LINE__);
            return AAA_FAIL;
        }

        mManualShutter = exp_time;

        if (to_hw)
        {
            fev = -1.0 * (log10(exp_time) / log10(2.0));
            ci_adv_err ret = ci_adv_ae_set_manual_shutter(CI_ADV_S15_16_FROM_FLOAT(fev));
            if(ci_adv_success != ret)
                return AAA_FAIL;

            LOGD(" *** manual set shutter in EV: %f\n", fev);
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetManualShutter(float *exp_time)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        int iev;
        ci_adv_err ret = ci_adv_ae_get_manual_shutter(&iev);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        *exp_time = pow(2, -1.0 * CI_ADV_S15_16_TO_FLOAT(iev));
        mManualShutter = *exp_time;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfSetManualFocus(int focus, bool to_hw)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        mFocusPosition = focus;

        if (to_hw && ci_adv_af_manual_focus_abs(focus) != 0)
            return AAA_FAIL;

        LOGD(" *** manual set focus distance in cm: %d\n", focus);
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfGetManualFocus(int *focus)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *focus = mFocusPosition;
    }

    return AAA_SUCCESS;

}

int AAAProcess::AfGetFocus(int *focus)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        struct v4l2_ext_controls controls;
        struct v4l2_ext_control control;

        controls.ctrl_class = V4L2_CTRL_CLASS_CAMERA;
        controls.count = 1;
        controls.controls = &control;
        control.id = V4L2_CID_FOCUS_ABSOLUTE;
        int ret = ioctl (main_fd, VIDIOC_S_EXT_CTRLS, &controls);
        LogDetail("line:%d, ret:%d, focus:%d", __LINE__, ret, *focus);
        *focus = control.value;

    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetWindow(const cam_Window *window)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_set_window((ci_adv_window *)window);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetWindow(cam_Window *window)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_get_window((ci_adv_window *)window);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbSetMode (int wb_mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_success;
        switch (wb_mode) {
        case CAM_AWB_MODE_DAYLIGHT:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_clear_sky);
            break;
        case CAM_AWB_MODE_CLOUDY:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_cloudiness);
            break;
        case CAM_AWB_MODE_SUNSET:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_filament_lamp);
            break;
        case CAM_AWB_MODE_TUNGSTEN:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_filament_lamp);
            break;
        case CAM_AWB_MODE_FLUORESCENT:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_fluorlamp_n);
            break;
        case CAM_AWB_MODE_WARM_FLUORESCENT:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_fluorlamp_w);
            break;
        case CAM_AWB_MODE_WARM_INCANDESCENT:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_filament_lamp);
            break;
        case CAM_AWB_MODE_SHADOW:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            ret = ci_adv_awb_set_light_source (ci_adv_awb_light_source_shadow_area);
            break;
        case CAM_AWB_MODE_MANUAL_INPUT:
            ci_adv_awb_set_mode (ci_adv_awb_mode_manual);
            break;
        case CAM_AWB_MODE_AUTO:
            ret = ci_adv_awb_set_mode (ci_adv_awb_mode_auto);
            break;
        default:
            LogError("Invalid set AWB mode!");
            ret = ci_adv_awb_set_mode (ci_adv_awb_mode_auto);
        }
        if (ret != ci_adv_success)
            return AAA_FAIL;
        mAwbMode = wb_mode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbGetMode(int *wb_mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *wb_mode = mAwbMode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbSetManualColorTemperature(int ct, bool to_hw)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        mColorTemperature = ct;

        if (to_hw)
        {
            int hw_ct;

            hw_ct = (ct < MIN_MANUAL_CT) ? MIN_MANUAL_CT : ((ct > MAX_MANUAL_CT) ? MAX_MANUAL_CT : ct);
            ci_adv_err ret = ci_adv_awb_set_manual_color_temperature(hw_ct);
            if(ci_adv_success != ret)
                return AAA_FAIL;
        }
        LOGD(" *** manual set color temperture in Kelvin: %d\n", ct);
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbGetManualColorTemperature(int *ct)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *ct = mColorTemperature;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetBacklightCorrection(bool en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_backlight_correction_mode wr_val;
        if (en == true)
        {
            wr_val = ci_adv_ae_backlight_correction_mode_on;
        }
        else
        {
            wr_val = ci_adv_ae_backlight_correction_mode_off;
        }
        ci_adv_err ret = ci_adv_ae_set_backlight_correction (wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetBacklightCorrection(bool *en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_backlight_correction_mode rd_val;
        ci_adv_err ret = ci_adv_ae_get_backlight_correction(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_ae_backlight_correction_mode_off:
            *en = false;
            break;
        case ci_adv_ae_backlight_correction_mode_on:
            *en = true;
            break;
        default:
            LogError("Invalid get AE backlight correction!");
            *en = false;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeGetExpCfg(unsigned short *exp_time, unsigned short *aperture)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_ae_get_exp_cfg(exp_time, aperture);
    }

    return AAA_SUCCESS;
}

int AAAProcess::SetRedEyeRemoval(bool en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_redeye_enable(en);
    }

    return AAA_SUCCESS;
}

int AAAProcess::GetRedEyeRemoval(bool *en)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *en = ci_adv_redeye_is_enabled ();
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbSetMapping(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_awb_map wr_val;
        switch (mode)
        {
        case CAM_AWB_MAP_AUTO:
            wr_val = ci_adv_awb_map_auto;
            break;
        case CAM_AWB_MAP_INDOOR:
            wr_val = ci_adv_awb_map_indoor;
            break;
        case CAM_AWB_MAP_OUTDOOR:
            wr_val = ci_adv_awb_map_outdoor;
            break;
        default:
            LogError("Invalid set AWB map mode!");
            wr_val = ci_adv_awb_map_auto;
        }
        ci_adv_err ret = ci_adv_awb_set_map (wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AwbGetMapping(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_awb_map rd_val;
        ci_adv_err ret = ci_adv_awb_get_map (&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_awb_map_indoor:
            *mode = CAM_AWB_MAP_INDOOR;
            break;
        case ci_adv_awb_map_outdoor:
            *mode = CAM_AWB_MAP_OUTDOOR;
            break;
        default:
            LogError("Invalid get AWB map mode!");
            *mode = CAM_AWB_MAP_INDOOR;
        }
    }

    return AAA_SUCCESS;

}

int AAAProcess::AfSetMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_success;

        switch (mode) {
        case CAM_AF_MODE_AUTO:
            ret = ci_adv_af_set_mode (ci_adv_af_mode_auto);
            ci_adv_af_set_range (ci_adv_af_range_norm);
            break;
        case CAM_AF_MODE_TOUCH:
            ret = ci_adv_af_set_mode (ci_adv_af_mode_auto);
            ci_adv_af_set_range (ci_adv_af_range_full);
            break;
        case CAM_AF_MODE_MACRO:
            ret = ci_adv_af_set_mode (ci_adv_af_mode_auto);
            ci_adv_af_set_range (ci_adv_af_range_macro);
            break;
        case CAM_AF_MODE_INFINITY:
            ret = ci_adv_af_set_mode (ci_adv_af_mode_manual);
            ci_adv_af_set_range (ci_adv_af_range_full);
            break;
        case CAM_AF_MODE_MANUAL:
            ret = ci_adv_af_set_mode (ci_adv_af_mode_manual);
            ci_adv_af_set_range (ci_adv_af_range_full);
            break;
        default:
            LogError("Invalid set AF mode!");
            ret = ci_adv_af_set_mode (ci_adv_af_mode_auto);
            ci_adv_af_set_range (ci_adv_af_range_norm);
            break;
        }
        if (ret != ci_adv_success)
            return AAA_FAIL;
        mAfMode = mode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfGetMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        *mode = mAfMode;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfSetMeteringMode(int mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_af_metering_mode wr_val;
        switch (mode)
        {
        case CAM_AF_METERING_MODE_AUTO:
            wr_val = ci_adv_af_metering_mode_auto;
            break;
        case CAM_AF_METERING_MODE_SPOT:
            wr_val = ci_adv_af_metering_mode_spot;
            break;
        default:
            LogError("Invalid set AF meter mode!");
            wr_val = ci_adv_af_metering_mode_auto;
        }
        ci_adv_err ret = ci_adv_af_set_metering_mode(wr_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfGetMeteringMode(int *mode)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_af_metering_mode rd_val;
        ci_adv_err ret = ci_adv_af_get_metering_mode(&rd_val);
        if(ci_adv_success != ret)
            return AAA_FAIL;
        switch (rd_val)
        {
        case ci_adv_af_metering_mode_auto:
            *mode = CAM_AF_METERING_MODE_AUTO;
            break;
        case ci_adv_af_metering_mode_spot:
            *mode = CAM_AF_METERING_MODE_SPOT;
            break;
        default:
            LogError("Invalid get AF meter mode!");
            *mode = CAM_AF_METERING_MODE_AUTO;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfSetWindow(const cam_Window *window)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_af_set_window((ci_adv_window *)window);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AfGetWindow(cam_Window *window)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return AAA_FAIL;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_af_get_window((ci_adv_window *)window);
        if(ci_adv_success != ret)
            return AAA_FAIL;
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeSetMeteringWeightMap(ci_adv_weight_map *weightmap)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if (!mInitied)
        return AAA_FAIL;

    if (SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_set_weight_map((const ci_adv_weight_map*) weightmap);
        if (ci_adv_success != ret) {
            LogError("Failed to set new weight map, ret = %d", ret);
            return AAA_FAIL;
        }
    }

    return AAA_SUCCESS;
}

// using AeGetMeteringWeight requires to use AeDestroyMeteringWeightMap
// whenever the weightmap is not used anymore

int AAAProcess::AeGetMeteringWeightMap(ci_adv_weight_map *weightmap)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if (!mInitied)
        return AAA_FAIL;
    if (SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_get_weight_map(&weightmap);
        if (ci_adv_success != ret) {
            LogError("Failed to get ae weight table, ret =%d", ret);
            return AAA_FAIL;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::AeDestroyMeteringWeightMap(ci_adv_weight_map *weightmap)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    if (!mInitied)
        return AAA_FAIL;
    if (SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_err ret = ci_adv_ae_destroy_weight_map(weightmap);
        if (ci_adv_success != ret) {
            LogError("Failed to destroy AE weight map, ret =%d", ret);
            return AAA_FAIL;
        }
    }

    return AAA_SUCCESS;
}

int AAAProcess::FlushManualSettings(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    int ret;

    // manual focus
    if (SENSOR_TYPE_RAW == mSensorType) {
        if (mAfMode == CAM_AF_MODE_MANUAL)
        {
            ret = AfSetManualFocus (mFocusPosition, true);

            if (ret != AAA_SUCCESS)
            {
                LogError("error in flush manual focus");
                return AAA_FAIL;
            }
        }
    }

    return AAA_SUCCESS;
}

/* private interface */
int AAAProcess::Init(const char *sensor_id, int fd)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);

    main_fd = fd;
    if (ci_adv_init(sensor_id, fd) == 0) {
        mSensorType = SENSOR_TYPE_RAW;
        mInitied = 1;
    } else {
        mSensorType = SENSOR_TYPE_SOC;
        mInitied = 0;
    }
    return mSensorType;
}

void AAAProcess::Uninit(void)
{
    LogEntry(LOG_TAG, __FUNCTION__);
    Mutex::Autolock lock(mLock);
    if(!mInitied)
        return;

    if(SENSOR_TYPE_RAW == mSensorType)
    {
        ci_adv_uninit();
        mInitied = 0;
    }
}

}; // namespace android

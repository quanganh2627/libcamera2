/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "Atom_EXIFMaker"

#include "EXIFMaker.h"
#include "LogHelper.h"
#include "AtomISP.h"
#include <camera.h>


namespace android {

EXIFMaker::EXIFMaker() :
        initialized(false)
{
    LOG1("@%s", __FUNCTION__);
}

EXIFMaker::~EXIFMaker()
{
    LOG1("@%s", __FUNCTION__);
}

void EXIFMaker::initialize(const CameraParameters &params)
{
    LOG1("@%s: params = %p", __FUNCTION__, &params);

    /* We clear the exif attributes, so we won't be using some old values
     * from a previous EXIF generation.
     */
    clear();

    // Initialize the exifAttributes with specific values
    // time information
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime((char *)exifAttributes.date_time, sizeof(exifAttributes.date_time), "%Y:%m:%d %H:%M:%S", timeinfo);

    // conponents configuration. 0 means does not exist
    // 1 = Y; 2 = Cb; 3 = Cr; 4 = R; 5 = G; 6 = B; other = reserved
    memset(exifAttributes.components_configuration, 0, sizeof(exifAttributes.components_configuration));

    // max aperture. the smallest F number of the lens. unit is APEX value.
    // TBD, should get from driver
    exifAttributes.max_aperture.num = exifAttributes.aperture.num;
    exifAttributes.max_aperture.den = exifAttributes.aperture.den;

    // subject distance,    0 means distance unknown; (~0) means infinity.
    exifAttributes.subject_distance.num = EXIF_DEF_SUBJECT_DISTANCE_UNKNOWN;
    exifAttributes.subject_distance.den = 1;

    // light source, 0 means light source unknown
    exifAttributes.light_source = 0;

    // gain control, 0 = none;
    // 1 = low gain up; 2 = high gain up; 3 = low gain down; 4 = high gain down
    exifAttributes.gain_control = 0;

    // sharpness, 0 = normal; 1 = soft; 2 = hard; other = reserved
    exifAttributes.sharpness = 0;

    // the picture's width and height
    params.getPictureSize((int*)&exifAttributes.width, (int*)&exifAttributes.height);

    thumbWidth = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    thumbHeight = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);

    int rotation = params.getInt(CameraParameters::KEY_ROTATION);
    exifAttributes.orientation = 1;
    if (0 == rotation)
        exifAttributes.orientation = 1;
    else if (90 == rotation)
        exifAttributes.orientation = 6;
    else if (180 == rotation)
        exifAttributes.orientation = 3;
    else if (270 == rotation)
        exifAttributes.orientation = 8;
    LOG1("EXIF: rotation value:%d degrees, orientation value:%d",
            rotation, exifAttributes.orientation);

    initializeHWSpecific();
    initializeLocation(params);

    initialized = true;
}

void EXIFMaker::initializeLocation(const CameraParameters &params)
{
    LOG1("@%s", __FUNCTION__);
    // GIS information
    bool gpsEnabled = true;
    const char *platitude = params.get(CameraParameters::KEY_GPS_LATITUDE);
    const char *plongitude = params.get(CameraParameters::KEY_GPS_LONGITUDE);
    const char *paltitude = params.get(CameraParameters::KEY_GPS_ALTITUDE);
    const char *ptimestamp = params.get(CameraParameters::KEY_GPS_TIMESTAMP);
    const char *pprocmethod = params.get(CameraParameters::KEY_GPS_PROCESSING_METHOD);

    // check whether the GIS Information is valid
    if((NULL == platitude) || (NULL == plongitude)
            || (NULL == paltitude) || (NULL == ptimestamp)
            || (NULL == pprocmethod))
        gpsEnabled = false;

    exifAttributes.enableGps = gpsEnabled;
    LOG1("EXIF: gpsEnabled: %d", gpsEnabled);

    if(gpsEnabled) {
        float latitude, longitude, altitude;
        long timestamp;
        unsigned len;
        struct tm time;

        // the version is given as 2.2.0.0, it is mandatory when GPSInfo tag is present
        const unsigned char gpsversion[4] = {0x02, 0x02, 0x00, 0x00};
        memcpy(exifAttributes.gps_version_id, gpsversion, sizeof(gpsversion));

        // latitude, for example, 39.904214 degrees, N
        latitude = atof(platitude);
        if(latitude > 0)
            memcpy(exifAttributes.gps_latitude_ref, "N", sizeof(exifAttributes.gps_latitude_ref));
        else
            memcpy(exifAttributes.gps_latitude_ref, "S", sizeof(exifAttributes.gps_latitude_ref));
        latitude = fabs(latitude);
        exifAttributes.gps_latitude[0].num = (uint32_t)latitude;
        exifAttributes.gps_latitude[0].den = 1;
        exifAttributes.gps_latitude[1].num = (uint32_t)((latitude - exifAttributes.gps_latitude[0].num) * 60);
        exifAttributes.gps_latitude[1].den = 1;
        exifAttributes.gps_latitude[2].num = (uint32_t)(((latitude - exifAttributes.gps_latitude[0].num) * 60 - exifAttributes.gps_latitude[1].num) * 60 * 100);
        exifAttributes.gps_latitude[2].den = 100;
        LOG1("EXIF: latitude, ref:%s, dd:%d, mm:%d, ss:%d",
                exifAttributes.gps_latitude_ref, exifAttributes.gps_latitude[0].num,
                exifAttributes.gps_latitude[1].num, exifAttributes.gps_latitude[2].num);

        // longitude, for example, 116.407413 degrees, E
        longitude = atof(plongitude);
        if(longitude > 0)
            memcpy(exifAttributes.gps_longitude_ref, "E", sizeof(exifAttributes.gps_longitude_ref));
        else
            memcpy(exifAttributes.gps_longitude_ref, "W", sizeof(exifAttributes.gps_longitude_ref));
        longitude = fabs(longitude);
        exifAttributes.gps_longitude[0].num = (uint32_t)longitude;
        exifAttributes.gps_longitude[0].den = 1;
        exifAttributes.gps_longitude[1].num = (uint32_t)((longitude - exifAttributes.gps_longitude[0].num) * 60);
        exifAttributes.gps_longitude[1].den = 1;
        exifAttributes.gps_longitude[2].num = (uint32_t)(((longitude - exifAttributes.gps_longitude[0].num) * 60 - exifAttributes.gps_longitude[1].num) * 60 * 100);
        exifAttributes.gps_longitude[2].den = 100;
        LOG1("EXIF: longitude, ref:%s, dd:%d, mm:%d, ss:%d",
                exifAttributes.gps_longitude_ref, exifAttributes.gps_longitude[0].num,
                exifAttributes.gps_longitude[1].num, exifAttributes.gps_longitude[2].num);

        // altitude, sea level or above sea level, set it to 0; below sea level, set it to 1
        altitude = atof(paltitude);
        exifAttributes.gps_altitude_ref = ((altitude > 0) ? 0 : 1);
        altitude = fabs(altitude);
        exifAttributes.gps_altitude.num = (uint32_t)altitude;
        exifAttributes.gps_altitude.den = 1;
        LOG1("EXIF: altitude, ref:%d, height:%d",
                exifAttributes.gps_altitude_ref, exifAttributes.gps_altitude.num);

        // timestampe
        timestamp = atol(ptimestamp);
        gmtime_r(&timestamp, &time);
        time.tm_year += 1900;
        time.tm_mon += 1;
        exifAttributes.gps_timestamp[0].num = time.tm_hour;
        exifAttributes.gps_timestamp[0].den = 1;
        exifAttributes.gps_timestamp[1].num = time.tm_min;
        exifAttributes.gps_timestamp[1].den = 1;
        exifAttributes.gps_timestamp[2].num = time.tm_sec;
        exifAttributes.gps_timestamp[2].den = 1;
        snprintf((char *)exifAttributes.gps_datestamp, sizeof(exifAttributes.gps_datestamp), "%04d:%02d:%02d",
                time.tm_year, time.tm_mon, time.tm_mday);
        LOG1("EXIF: timestamp, year:%d,mon:%d,day:%d,hour:%d,min:%d,sec:%d",
                time.tm_year, time.tm_mon, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec);

        // processing method
        if(strlen(pprocmethod) + 1 >= sizeof(exifAttributes.gps_processing_method))
            len = sizeof(exifAttributes.gps_processing_method);
        else
            len = strlen(pprocmethod) + 1;
        memcpy(exifAttributes.gps_processing_method, pprocmethod, len);
        LOG1("EXIF: GPS processing method:%s", exifAttributes.gps_processing_method);
    }
}

void EXIFMaker::initializeHWSpecific()
{
    LOG1("@%s", __FUNCTION__);
#ifdef ANDROID_1598
    // exp_time's unit is 100us
    mAAA->AeGetExpCfg(&exp_time, &aperture);
    // exposure time
    exifAttributes.exposure_time.num = exp_time;
    exifAttributes.exposure_time.den = 10000;

    // shutter speed, = -log2(exposure time)
    float exp_t = (float)(exp_time / 10000.0);
    float shutter = -1.0 * (log10(exp_t) / log10(2.0));
    exifAttributes.shutter_speed.num = (shutter * 10000);
    exifAttributes.shutter_speed.den = 10000;

    // fnumber
    // TBD, should get from driver
    ret = mCamera->getFnumber(&fnumber);
    if (ret < 0) {
        // Error handler: if driver does not support Fnumber achieving, just give the default value.
        exifAttributes.fnumber.num = EXIF_DEF_FNUMBER_NUM;
        exifAttributes.fnumber.den = EXIF_DEF_FNUMBER_DEN;
        ret = 0;
    } else {
        exifAttributes.fnumber.num = fnumber >> 16;
        exifAttributes.fnumber.den = fnumber & 0xffff;
        LogDetail("fnumber:%x, num: %d, den: %d", fnumber, exifAttributes.fnumber.num, exifAttributes.fnumber.den);
    }

    // aperture
    exifAttributes.aperture.num = 100*(int)((1.0*exifAttributes.fnumber.num/exifAttributes.fnumber.den) * sqrt(100.0/aperture));
    exifAttributes.aperture.den = 100;

    if (mSensorType == SENSOR_TYPE_RAW) {
        // brightness, -99.99 to 99.99. FFFFFFFF.H means unknown.
        float brightness;
        mAAA->AeGetManualBrightness(&brightness);
        attribute.brightness.num = (int)(brightness*100);
        attribute.brightness.den = 100;

        // exposure bias. unit is APEX value. -99.99 to 99.99
        float bias;
        mAAA->AeGetEv(&bias);
        attribute.exposure_bias.num = (int)(bias * 100);
        attribute.exposure_bias.den = 100;
        LogDetail("brightness:%f, ev:%f", brightness, bias);

        // set the exposure program mode
        int aemode;
        if (AAA_SUCCESS == mAAA->AeGetMode(&aemode)) {
            switch (aemode) {
            case CAM_AE_MODE_MANUAL:
                attribute.exposure_program = EXIF_EXPOSURE_PROGRAM_MANUAL;
                break;
            case CAM_AE_MODE_SHUTTER_PRIORITY:
                attribute.exposure_program = EXIF_EXPOSURE_PROGRAM_SHUTTER_PRIORITY;
                break;
            case CAM_AE_MODE_APERTURE_PRIORITY:
                attribute.exposure_program = EXIF_EXPOSURE_PROGRAM_APERTURE_PRIORITY;
                break;
            case CAM_AE_MODE_AUTO:
            default:
                attribute.exposure_program = EXIF_EXPOSURE_PROGRAM_NORMAL;
                break;
            }
        } else {
            attribute.exposure_program = EXIF_EXPOSURE_PROGRAM_NORMAL;
        }

        // indicates the ISO speed of the camera
        int sensitivity;
        if (AAA_SUCCESS == mAAA->AeGetManualIso(&sensitivity)) {
            attribute.iso_speed_rating = sensitivity;
        } else {
            LogDetail("AeGetManualIso failed!");
            attribute.iso_speed_rating = 100;
        }

        // the metering mode.
        int meteringmode;
        if (AAA_SUCCESS == mAAA->AeGetMeteringMode(&meteringmode)) {
            switch (meteringmode) {
            case CAM_AE_METERING_MODE_AUTO:
                attribute.metering_mode = EXIF_METERING_AVERAGE;
                break;
            case CAM_AE_METERING_MODE_SPOT:
                attribute.metering_mode = EXIF_METERING_SPOT;
                break;
            case CAM_AE_METERING_MODE_CENTER:
                attribute.metering_mode = EXIF_METERING_CENTER;
                break;
            case CAM_AE_METERING_MODE_CUSTOMIZED:
            default:
                attribute.metering_mode = EXIF_METERING_OTHER;
                break;
            }
        } else {
            attribute.metering_mode = EXIF_METERING_OTHER;
        }

        // exposure mode settting. 0: auto; 1: manual; 2: auto bracket; other: reserved
        if (AAA_SUCCESS == mAAA->AeGetMode(&ae_mode)) {
            LogDetail("exifAttribute, ae mode:%d success", ae_mode);
            switch (ae_mode) {
            case CAM_AE_MODE_MANUAL:
                attribute.exposure_mode = EXIF_EXPOSURE_MANUAL;
                break;
            default:
                attribute.exposure_mode = EXIF_EXPOSURE_AUTO;
                break;
            }
        } else {
            attribute.exposure_mode = EXIF_EXPOSURE_AUTO;
        }

        // white balance mode. 0: auto; 1: manual
        int awbmode;
        if(AAA_SUCCESS == mAAA->AwbGetMode(&awbmode)) {
            switch (awbmode) {
            case CAM_AWB_MODE_AUTO:
                attribute.white_balance = EXIF_WB_AUTO;
                break;
            default:
                attribute.white_balance = EXIF_WB_MANUAL;
                break;
            }
        } else {
            attribute.white_balance = EXIF_WB_AUTO;
        }

        // scene mode
        int scenemode;
        if (AAA_SUCCESS == mAAA->AeGetSceneMode(&scenemode)) {
            switch (scenemode) {
            case CAM_AE_SCENE_MODE_PORTRAIT:
                attribute.scene_capture_type = EXIF_SCENE_PORTRAIT;
                break;
            case CAM_AE_SCENE_MODE_LANDSCAPE:
                attribute.scene_capture_type = EXIF_SCENE_LANDSCAPE;
                break;
            case CAM_AE_SCENE_MODE_NIGHT:
                attribute.scene_capture_type = EXIF_SCENE_NIGHT;
                break;
            default:
                attribute.scene_capture_type = EXIF_SCENE_STANDARD;
                break;
            }
        } else {
            attribute.scene_capture_type = EXIF_SCENE_STANDARD;
        }
    }

    // the actual focal length of the lens, in mm.
    // there is no API for lens position.
    ret = mCamera->getFocusLength(&focal_length);
    if (ret < 0) {
        // Error handler: if driver does not support focal_length achieving, just give the default value.
        exifAttributes.focal_length.num = EXIF_DEF_FOCAL_LEN_NUM;
        exifAttributes.focal_length.den = EXIF_DEF_FOCAL_LEN_DEN;
        ret = 0;
    } else {
        exifAttributes.focal_length.num = focal_length >> 16;
        exifAttributes.focal_length.den = focal_length & 0xffff;
        LogDetail("line:%d, focal_length:%x, num: %d, den: %d", __LINE__, focal_length, exifAttributes.focal_length.num, exifAttributes.focal_length.den);
    }
#endif
}

void EXIFMaker::clear()
{
    LOG1("@%s", __FUNCTION__);
    // Reset all the attributes
    memset(&exifAttributes, 0, sizeof(exifAttributes));

    // Initialize the common values
    exifAttributes.enableThumb = false;
    strncpy((char*)exifAttributes.image_description, EXIF_DEF_IMAGE_DESCRIPTION, sizeof(exifAttributes.image_description));
    strncpy((char*)exifAttributes.maker, EXIF_DEF_MAKER, sizeof(exifAttributes.maker));
    strncpy((char*)exifAttributes.model, EXIF_DEF_MODEL, sizeof(exifAttributes.model));
    strncpy((char*)exifAttributes.software, EXIF_DEF_SOFTWARE, sizeof(exifAttributes.software));

    memcpy(exifAttributes.exif_version, EXIF_DEF_EXIF_VERSION, sizeof(exifAttributes.exif_version));
    memcpy(exifAttributes.flashpix_version, EXIF_DEF_FLASHPIXVERSION, sizeof(exifAttributes.flashpix_version));

    // initially, set default flash
    exifAttributes.flash = EXIF_DEF_FLASH;

    // normally it is sRGB, 1 means sRGB. FFFF.H means uncalibrated
    exifAttributes.color_space = EXIF_DEF_COLOR_SPACE;

    // the number of pixels per ResolutionUnit in the w or h direction
    // 72 means the image resolution is unknown
    exifAttributes.x_resolution.num = EXIF_DEF_RESOLUTION_NUM;
    exifAttributes.x_resolution.den = EXIF_DEF_RESOLUTION_DEN;
    exifAttributes.y_resolution.num = exifAttributes.x_resolution.num;
    exifAttributes.y_resolution.den = exifAttributes.x_resolution.den;
    // resolution unit, 2 means inch
    exifAttributes.resolution_unit = EXIF_DEF_RESOLUTION_UNIT;
    // when thumbnail uses JPEG compression, this tag 103H's value is set to 6
    exifAttributes.compression_scheme = EXIF_DEF_COMPRESSION;

    // the TIFF default is 1 (centered)
    exifAttributes.ycbcr_positioning = EXIF_DEF_YCBCR_POSITIONING;

    initialized = false;
}

void EXIFMaker::enableFlash()
{
    LOG1("@%s", __FUNCTION__);
    // bit 0: flash fired; bit 1 to 2: flash return; bit 3 to 4: flash mode;
    // bit 5: flash function; bit 6: red-eye mode;
    exifAttributes.flash = EXIF_FLASH_ON;
}

void EXIFMaker::setThumbnail(unsigned char *data, size_t size)
{
    LOG1("@%s: data = %p, size = %u", __FUNCTION__, data, size);
    exifAttributes.enableThumb = true;
    exifAttributes.widthThumb = thumbWidth;
    exifAttributes.heightThumb = thumbHeight;
    encoder.setThumbData(data, size);
}

size_t EXIFMaker::makeExif(unsigned char **data)
{
    LOG1("@%s", __FUNCTION__);
    if (*data == NULL) {
        LOGE("NULL pointer passed for EXIF. Cannot generate EXIF!");
        return 0;
    }
    if (encoder.makeExif(*data, &exifAttributes, &exifSize, false) == JPG_SUCCESS) {
        LOG1("Generated EXIF (@%p) of size: %u", *data, exifSize);
        return exifSize;
    }
    return 0;
}

}; // namespace android
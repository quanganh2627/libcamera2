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

#ifndef LOG_TAG
#define LOG_TAG "Camera_Dump"
#endif
#include <stdint.h> // INT_MAX, INT_MIN
#include <stdlib.h> // atoi.h
#include <utils/Log.h>
#include <cutils/properties.h>
#include <sys/stat.h>
#include "LogHelper.h"
#include "CameraDump.h"
#include "ia_aiq_types.h"

namespace android {

#define GIDSETSIZE      20

const char rawdp[][DUMPIMAGE_RAWDPPATHSIZE] = {
    DUMPIMAGE_SD_INT_PATH,
    DUMPIMAGE_MEM_INT_PATH,
    DUMPIMAGE_SD_EXT_PATH
};

const char *filename[RAW_OVER] = {
    DUMPIMAGE_RAW_NONE_FILENAME,
    DUMPIMAGE_RAW_YUV_FILENAME,
    DUMPIMAGE_RAW_BAYER_FILENAME
};

CameraDump* CameraDump::sInstance = NULL;
raw_data_format_E CameraDump::sRawDataFormat = RAW_NONE;
bool CameraDump::sNeedDumpPreview = false;
bool CameraDump::sNeedDumpSnapshot = false;
bool CameraDump::sNeedDumpVideo = false;
bool CameraDump::sNeedDump3aStat = false;

CameraDump::CameraDump()
{
    LOG1("@%s", __FUNCTION__);
    mDelayDump.buffer_raw = NULL;
    mDelayDump.buffer_size = 0;
    mDelayDump.width = 0;
    mDelayDump.height = 0;
    mNeedDumpFlush = false;
}

CameraDump::~CameraDump()
{
    LOG1("@%s", __FUNCTION__);
    sInstance = NULL;
    sRawDataFormat = RAW_NONE;
    sNeedDumpPreview = false;
    sNeedDumpVideo = false;
    sNeedDumpSnapshot = false;
    if (mDelayDump.buffer_raw) {
        free(mDelayDump.buffer_raw);
        mDelayDump.buffer_raw = NULL;
    }
}

void CameraDump::setDumpDataFlag(void)
{
    LOG1("@%s", __FUNCTION__);
    char DumpLevelProp[PROPERTY_VALUE_MAX];

    sRawDataFormat = RAW_NONE;
    sNeedDumpPreview = false;
    sNeedDumpVideo = false;
    sNeedDumpSnapshot = false;
    sNeedDump3aStat = false;

    // Set the dump debug level from property:
    if (property_get("camera.hal.debug", DumpLevelProp, NULL)) {
        int DumpProp = atoi(DumpLevelProp);

        // Check that the property value is a valid integer
        if (DumpProp >= INT_MAX || DumpProp <= INT_MIN) {
            LOGE("Invalid camera.hal.debug property integer value.");
            return ;
        }
        if (DumpProp & CAMERA_DEBUG_DUMP_RAW)
            sRawDataFormat = RAW_BAYER;

        if (DumpProp & CAMERA_DEBUG_DUMP_YUV)
            sRawDataFormat = RAW_YUV;

        if (DumpProp & CAMERA_DEBUG_DUMP_PREVIEW)
            sNeedDumpPreview = true;

        if (DumpProp & CAMERA_DEBUG_DUMP_VIDEO)
            sNeedDumpVideo = true;

        if (DumpProp & CAMERA_DEBUG_DUMP_SNAPSHOT)
            sNeedDumpSnapshot = true;

        if (DumpProp & CAMERA_DEBUG_DUMP_3A_STATISTICS)
            sNeedDump3aStat = true;
    }
    LOG1("sRawDataFormat=%d, sNeedDumpPreview=%d, sNeedDumpVideo=%d, sNeedDumpSnapshot=%d",
         sRawDataFormat, sNeedDumpPreview, sNeedDumpVideo, sNeedDumpSnapshot);
}

void CameraDump::setDumpDataFlag(int dumpFlag)
{
    LOG1("@%s", __FUNCTION__);

    if (dumpFlag == CAMERA_DEBUG_DUMP_RAW)
        sRawDataFormat = RAW_BAYER;
    else if (dumpFlag == CAMERA_DEBUG_DUMP_YUV)
        sRawDataFormat = RAW_YUV;
    else
        sRawDataFormat = RAW_NONE;
}

bool CameraDump::isDumpImageEnable(int dumpFlag)
{
    bool ret = false;
    switch (dumpFlag) {
        case CAMERA_DEBUG_DUMP_RAW:
            ret = (sRawDataFormat == RAW_BAYER);
            break;
        case CAMERA_DEBUG_DUMP_YUV:
            ret = (sRawDataFormat == RAW_YUV);
            break;
        case CAMERA_DEBUG_DUMP_PREVIEW:
            ret = sNeedDumpPreview;
            break;
        case CAMERA_DEBUG_DUMP_VIDEO:
            ret = sNeedDumpVideo;
            break;
        case CAMERA_DEBUG_DUMP_SNAPSHOT:
            ret = sNeedDumpSnapshot;
            break;
        case CAMERA_DEBUG_DUMP_3A_STATISTICS:
            ret = sNeedDump3aStat;
            break;
        default:
            ret = false;
        break;
    }
    return ret;
}

int CameraDump::dumpImage2Buf(void *buffer, unsigned int size, unsigned int width, unsigned int height)
{
    LOG1("@%s", __FUNCTION__);
    if ((0 == size) || (0 == width) || (0 == height)) {
        LOGE("value is err(size=%d,width=%d,height=%d)", size, width, height);
        return -ERR_D2F_EVALUE;
    }
    if ((mDelayDump.buffer_raw != NULL) && (size > mDelayDump.buffer_size)) {
        free(mDelayDump.buffer_raw);
        mDelayDump.buffer_raw = NULL;
    }
    if (!mDelayDump.buffer_raw) {
        LOG1 ("Buffer allocate needed %d", size);
        mDelayDump.buffer_raw = malloc(size);
        if (mDelayDump.buffer_raw  == NULL) {
            LOGE("Buffer allocate failure");
            mDelayDump.buffer_size = 0;
            mDelayDump.width = 0;
            mDelayDump.height = 0;
            mNeedDumpFlush = false;
            return -ERR_D2F_NOMEM;
        }
    }
    mDelayDump.buffer_size = size;
    mDelayDump.width = width;
    mDelayDump.height = height;
    memcpy(mDelayDump.buffer_raw, buffer, size);
    mNeedDumpFlush = true;

    return ERR_D2F_SUCESS;
}

int CameraDump::dumpImage2File(const void *data, const unsigned int size, unsigned int width,
                          unsigned int height, const char *name)
{
    LOG1("@%s", __FUNCTION__);
    char filename[80];
    static unsigned int count = 0;
    size_t bytes;
    FILE *fp;
    ia_3a_mknote * uMknData = NULL;
    char rawdpp[100];
    int ret;

    if ((NULL == data) || (0 == size) || (0 == width) || (0 == height) || (NULL == name)
        || (NULL == m3AControls))
        return -ERR_D2F_EVALUE;

    LOG2("%s filename is %s", __func__, name);
    /* media server may not have the access to SD card */
    showMediaServerGroup();

    ret = getRawDataPath(rawdpp);
    LOG2("RawDataPath is %s", rawdpp);
    if(-ERR_D2F_NOPATH == ret) {
        LOGE("%s No valid mem for rawdata", __func__);
        return ret;
    }
    if ((strcmp(name, "raw.bayer") == 0) && (m3AControls != NULL))
    {
        /* Only RAW image will have same file name as JPEG */
        char filesuffix[20];
        time_t rawtime;
        struct tm *timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        strftime((char *)filename, sizeof(filename), "IMG_%Y%m%d_%H%M%S", timeinfo);
        snprintf(filesuffix, sizeof(filesuffix), "%03u.i3av4", count);
        strncat(filename, filesuffix, sizeof(filename) - strlen(filename) - 1);
        strncat(rawdpp, filename, strlen(filename));

        ia_aiq_raw_image_full_info raw_info;
        raw_info.raw_image.data_format = ia_aiq_data_format_rawplain16;
        raw_info.raw_image.bayer_order = ia_aiq_bayer_order_grbg;
        raw_info.raw_image.data_format_bpp = 16;
        raw_info.raw_image.data_bpp = 10;
        raw_info.raw_image.width_cols = width;
        raw_info.raw_image.height_lines = height;
        raw_info.header_size_bytes = 0;
        raw_info.footer_size_bytes = 0;
        raw_info.extra_bytes_left = 0;
        raw_info.extra_bytes_right = 0;
        raw_info.extra_lines_top = 0;
        raw_info.extra_cols_left = 0;
        raw_info.extra_cols_right = 0;
        raw_info.extra_lines_bottom = 0;
        raw_info.byte_order_xor = 0;
        raw_info.spatial_sampling = 0;

        // Add raw image info to the maker note.
        m3AControls->add3aMakerNoteRecord(ia_3a_mknote_field_type_uint8, ia_3a_mknote_field_name_raw_info, &raw_info, sizeof(ia_aiq_raw_image_full_info));

        // Get maker note data
        uMknData = m3AControls->get3aMakerNote(ia_3a_mknote_mode_raw);
        if (uMknData) {
            LOGD("RAW, mknSize: %d", uMknData->bytes);
        } else {
            LOGW("RAW, no makernote");
        }
    }
    else
    {
        snprintf(filename, sizeof(filename), "dump_%d_%d_%03u_%s", width,
                 height, count, name);
        strncat(rawdpp, filename, strlen(filename));
    }

    fp = fopen (rawdpp, "w+");
    if (fp == NULL) {
        LOGE("open file %s failed %s", rawdpp, strerror(errno));
        if (uMknData) {
            // Delete Maker note data
            m3AControls->put3aMakerNote(uMknData);
        }
        return -ERR_D2F_EOPEN;
    }

    LOG1("Begin write image %s", filename);

    if (uMknData && uMknData->bytes > 0)
    {
        if ((bytes = fwrite(uMknData->data, uMknData->bytes, 1, fp)) < (size_t)uMknData->bytes)
            LOGW("Write less mkn bytes to %s: %d, %d", filename, uMknData->bytes, bytes);
    }

    if ((bytes = fwrite(data, size, 1, fp)) < (size_t)size)
        LOGW("Write less raw bytes to %s: %d, %d", filename, size, bytes);

    count++;

    if (uMknData)
    {
        // Delete Maker note data
        m3AControls->put3aMakerNote(uMknData);
    }

    fclose (fp);

    return ERR_D2F_SUCESS;
}

int CameraDump::dumpImage2FileFlush(bool bufflag)
{
    LOG1("@%s", __FUNCTION__);
    int ret = ERR_D2F_SUCESS;
    int nameID;

    if (mNeedDumpFlush == true) {
        if ((NULL == mDelayDump.buffer_raw) || (0 == mDelayDump.buffer_size)
            || (0 == mDelayDump.width) || (0 == mDelayDump.height))
            return -ERR_D2F_EVALUE;

        if (isDumpImageEnable(CAMERA_DEBUG_DUMP_YUV))
            nameID = RAW_YUV;
        else if (isDumpImageEnable(CAMERA_DEBUG_DUMP_RAW))
            nameID = RAW_BAYER;
        else
            nameID = RAW_NONE;

        ret = dumpImage2File(mDelayDump.buffer_raw, mDelayDump.buffer_size, mDelayDump.width,
                         mDelayDump.height,filename[nameID]);

        if(bufflag){
            free(mDelayDump.buffer_raw);
            mDelayDump.buffer_raw = NULL;
            mDelayDump.buffer_size = 0;
            mDelayDump.width = 0;
            mDelayDump.height = 0;
        }
        mNeedDumpFlush = false;
    }

    return ret;
}

int CameraDump::getRawDataPath(char *ppath)
{
    LOG1("@%s", __FUNCTION__);
    int pathcount = sizeof(rawdp) / sizeof(rawdp[0]);
    struct stat buf;
    int i;
    /* for now just check exist, no access, no space calc */
    for(i = 0; i < pathcount; i++) {
        LOG2("%s", (char *)(rawdp+i));
        if(stat((char *)(rawdp+i), &buf) < 0) {
            LOGE("stat %s failed %s", (char *)(rawdp+i), strerror(errno));
            continue;
        }
        if(S_ISDIR(buf.st_mode)) {
            break;
        }
    }
    if(i < pathcount) {
        strcpy(ppath, (char *)(rawdp+i));
        return ERR_D2F_SUCESS;
    }
    else {
        return -ERR_D2F_NOPATH;
    }
}

void CameraDump::showMediaServerGroup(void)
{
    LOG1("@%s", __FUNCTION__);
    gid_t grouplist[GIDSETSIZE];
    int x = getgroups(0, grouplist);
    getgroups(x, grouplist);
    if(x < GIDSETSIZE) {
        int y;
        for(y = 0; y < x; y++) {
            LOGI("MediaServer GrpID-%d:%d", y , grouplist[y]);
        }
    }
    else
        LOGE("%s Not enough mem for %d groupid", __func__, GIDSETSIZE);
}

void CameraDump::set3AControls(I3AControls *aaaControls)
{
    LOG1("@%s", __FUNCTION__);
    m3AControls = aaaControls;
}

}; // namespace android

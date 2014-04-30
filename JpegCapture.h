/*
 * Copyright (c) 2014 Intel Corporation
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

#ifndef ANDROID_LIBCAMERA_JPEG_CAPTURE_H
#define ANDROID_LIBCAMERA_JPEG_CAPTURE_H

#include <stdint.h>
#include <endian.h>

#define V4L2_PIX_FMT_CONTINUOUS_JPEG V4L2_PIX_FMT_JPEG

namespace android {

const int NUM_OF_JPEG_CAPTURE_SNAPSHOT_BUF = 6;
const int FMT_CONTINUOUS_JPEG_BPL = 2048;

// Frame size and place
const size_t JPEG_INFO_START = 2048;
const size_t JPEG_INFO_SIZE = 2048;
const size_t NV12_META_START = JPEG_INFO_START + JPEG_INFO_SIZE;
const size_t NV12_META_SIZE = 4096;
const size_t JPEG_META_START = NV12_META_START + NV12_META_SIZE;
const size_t JPEG_META_SIZE = 4096;
const size_t JPEG_DATA_START = JPEG_META_START + JPEG_META_SIZE;
const size_t JPEG_DATA_SIZE = 0x800000;
const size_t JPEG_FRAME_SIZE = JPEG_DATA_START + JPEG_DATA_SIZE;

// JPEG INFO addresses
const size_t JPEG_INFO_START_MARKER_ADDR = 0x0;
const size_t JPEG_INFO_MODE_ADDR = 0xF;
const size_t JPEG_INFO_COUNT_ADDR = 0x10;
const size_t JPEG_INFO_SIZE_ADDR = 0x13;
const size_t JPEG_INFO_YUV_FRAME_ID_ADDR = 0x17;
const size_t JPEG_INFO_THUMBNAIL_FRAME_ID_ADDR = 0x1B;
const size_t JPEG_INFO_END_MARKER_ADDR = 0x1F;

// JPEG INFO data
const char JPEG_INFO_START_MARKER[] =  "JPEG INFO-START";
const char JPEG_INFO_END_MARKER[] = "JPEG INFO-END";

enum JpegFrameType {
    JPEG_FRAME_TYPE_META = 0x00,
    JPEG_FRAME_TYPE_FULL = 0x01,
    JPEG_FRAME_TYPE_SPLIT = 0x02
};

// NV12 META addresses
const size_t NV12_META_START_MARKER_ADDR = 0x0;
const size_t NV12_META_FRAME_COUNT_ADDR = 0xE;
const size_t NV12_META_ISO_ADDR = 0x1C;
const size_t NV12_META_EXPOSURE_BIAS_VALUE_ADDR = 0X2C;
const size_t NV12_META_EXPOSURE_TIME_DENOMINATOR_ADDR = 0X2C;
const size_t NV12_META_AF_STATE_ADDR = 0x846;
const size_t NV12_META_END_MARKER_ADDR = 0xFF4;


// NV12 META data
const char NV12_META_START_MARKER[] =  "METADATA-START";
const char NV12_META_END_MARKER[] = "METADATA-END";

// JPEG META addresses
const size_t JPEG_META_FRAME_COUNT_ADDR = 0x13;

static uint32_t getU32fromFrame(uint8_t* framePtr, size_t addr) {

    uint32_t result = *((uint32_t*)(framePtr + addr));

    result = be32toh(result);

    return result;
}

} // namespace

#endif // ANDROID_LIBCAMERA_JPEG_CAPTURE_H

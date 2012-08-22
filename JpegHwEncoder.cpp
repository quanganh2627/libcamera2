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
#define LOG_TAG "Camera_JpegHwEncoder"

#include "JpegCompressor.h"
#include "LogHelper.h"
#include "JpegHwEncoder.h"
#include "vaJpegContext.h"

namespace android {

JpegHwEncoder::JpegHwEncoder() :
    mVaEncoderContext(NULL),
    mHWInitialized(false),
    mPicWidth(0),
    mPicHeight(0),
    mMaxOutJpegBufSize(0)
{
    LOG1("@%s", __FUNCTION__);
    mVaEncoderContext = new vaJpegContext();

}

JpegHwEncoder::~JpegHwEncoder()
{
    LOG1("@%s", __FUNCTION__);
    if(mHWInitialized)
        deInit();
    if(mVaEncoderContext != NULL)
        delete mVaEncoderContext;
}

int JpegHwEncoder::init(void)
{
    LOG1("@%s", __FUNCTION__);
    VAStatus status;
    int display_num, major_ver, minor_ver;
    int num_entrypoints, i, maxNum;
    const char *driver = NULL;
    VAEntrypoint entrypoints[VAEntrypointMax];
    VAConfigAttrib attrib;
    vaJpegContext *va;

    if (mVaEncoderContext == NULL) {
        LOGE("Failed to create VA encoder context struct, no memory");
        mHWInitialized = false;
        return -1;
    }

    va = mVaEncoderContext;
    va->mDpy = vaGetDisplay(&display_num);
    status = vaInitialize(va->mDpy, &major_ver, &minor_ver);
    CHECK_STATUS(status, "vaInitialize", __LINE__)

    driver = vaQueryVendorString(va->mDpy);
    maxNum = vaMaxNumEntrypoints(va->mDpy);
    status = vaQueryConfigEntrypoints(va->mDpy, VAProfileJPEGBaseline,
                                      entrypoints, &num_entrypoints);
    CHECK_STATUS(status, "vaQueryConfigEntrypoints", __LINE__)

    for(i = 0; i < num_entrypoints; i++) {
        if (entrypoints[i] == VAEntrypointEncPicture)
            break;
    }
    if (i == num_entrypoints) {
        LOGE("@%s, line:%d, not find Slice entry point, num:%d",
                __FUNCTION__, __LINE__, num_entrypoints);
        return -1;
    }

    attrib.type = VAConfigAttribRTFormat;
    attrib.value = va->mSupportedFormat;
    status = vaCreateConfig(va->mDpy, VAProfileJPEGBaseline, VAEntrypointEncPicture,
                            &attrib, 1, &va->mConfigId);
    CHECK_STATUS(status, "vaCreateConfig", __LINE__)

    mHWInitialized = true;
    return 0;
}

void JpegHwEncoder::deInit()
{
    LOG1("@%s", __FUNCTION__);
    vaJpegContext *va = mVaEncoderContext;

    if(va->mBuff2SurfId.size() != 0)
        destroySurfaces();

    if (va->mDpy && va->mConfigId)
        vaDestroyConfig(va->mDpy, va->mConfigId);
    if (va->mDpy)
        vaTerminate(va->mDpy);

    mHWInitialized = false;
}

status_t JpegHwEncoder::setInputBuffer(const JpegCompressor::InputBuffer &inBuf)
{
    LOG1("@%s", __FUNCTION__);

    VAStatus status;
    void *surface_p = NULL;
    vaJpegContext *va = mVaEncoderContext;
    VAEncPictureParameterBufferJPEG pic_jpeg;
    VASurfaceAttributeTPI           surfaceAttrib;
    memset(&surfaceAttrib, 0, sizeof(surfaceAttrib));

    if(va->mBuff2SurfId.size() != 0)
        destroySurfaces();

    mPicWidth = inBuf.width;
    mPicHeight = inBuf.height;
    mMaxOutJpegBufSize = inBuf.size;

    if (mPicHeight % 2) {
        LOG1("@%s, line:%d, height:%d, we can't support", __FUNCTION__, __LINE__, mPicHeight);
        return -1;
    }

    surfaceAttrib.buffers = (unsigned int *)&inBuf.buf;
    surfaceAttrib.count = 1;
    surfaceAttrib.luma_stride = mPicWidth;
    surfaceAttrib.pixel_format = VA_FOURCC_NV12;
    surfaceAttrib.width = mPicWidth;
    surfaceAttrib.height = mPicHeight;
    surfaceAttrib.type = VAExternalMemoryUserPointer;
    status = vaCreateSurfacesWithAttribute(va->mDpy, mPicWidth, mPicHeight, va->mSupportedFormat,
                                           1, va->mSurfaceIds, &surfaceAttrib);
    CHECK_STATUS(status, "vaCreateSurfacesWithAttribute", __LINE__)


    status = vaCreateContext(va->mDpy, va->mConfigId, mPicWidth, mPicHeight,
                             VA_PROGRESSIVE, va->mSurfaceIds, 1, &va->mContextId);
    CHECK_STATUS(status, "vaCreateContext", __LINE__)

    /* Create mapping vector from buffer address to surface id */
    va->mBuff2SurfId.add((unsigned int)inBuf.buf, va->mSurfaceIds[0]);


    /* Allocate buffer for compressed  output. It is stored in mCodedBuf */
    status = vaCreateBuffer(va->mDpy, va->mContextId, VAEncCodedBufferType,
                            mMaxOutJpegBufSize, 1, NULL, &va->mCodedBuf);
    CHECK_STATUS(status, "vaCreateBuffer", __LINE__)

    va->mCurrentSurface = 0;

    return NO_ERROR;
}

int JpegHwEncoder::encode(const JpegCompressor::InputBuffer &in, JpegCompressor::OutputBuffer &out)
{
    LOG1("@%s", __FUNCTION__);
    int status;
    VASurfaceID aSurface = 0;
    VAEncPictureParameterBufferJPEG pic_jpeg;
    vaJpegContext *va = mVaEncoderContext;

    LOG1("input buffer address: %p", in.buf);

    aSurface = va->mBuff2SurfId.valueFor((unsigned int)in.buf);
    if(aSurface == 0) {
        LOGW("Received buffer does not map to any surface");
        return -1;
    }

    pic_jpeg.picture_width  = mPicWidth;
    pic_jpeg.picture_height = mPicHeight;
    pic_jpeg.reconstructed_picture = 0;
    pic_jpeg.coded_buf = va->mCodedBuf;
    status = vaCreateBuffer(va->mDpy, va->mContextId, VAEncPictureParameterBufferType,
                            sizeof(pic_jpeg), 1, &pic_jpeg, &va->mPicParamBuf);
    CHECK_STATUS(status, "vaCreateBuffer", __LINE__)

    status = setJpegQuality(out.quality);
    if (status)
        goto exit;

    status = startJpegEncoding(aSurface);
    if (status)
        goto exit;

    status = vaSyncSurface(va->mDpy, aSurface);
    CHECK_STATUS(status, "vaSyncSurface", __LINE__)

    status = getJpegData(out.buf, out.size, &out.length);

exit:
    return (status ? -1 : 0);
}

int JpegHwEncoder::encodeAsync(const JpegCompressor::InputBuffer &in, JpegCompressor::OutputBuffer &out)
{
    LOG1("@%s", __FUNCTION__);
    int status = 0;
    VASurfaceID aSurface = 0;
    VAEncPictureParameterBufferJPEG pic_jpeg;
    vaJpegContext *va = mVaEncoderContext;

    LOG1("input buffer address: %p", in.buf);

    aSurface = va->mBuff2SurfId.valueFor((unsigned int)in.buf);
    if(aSurface == 0) {
        LOGW("Received buffer does not map to any surface");
        return -1;
    }

    pic_jpeg.picture_width  = mPicWidth;    //TODO: Here we should get the dimensions from OutputBuffer
    pic_jpeg.picture_height = mPicHeight;
    pic_jpeg.reconstructed_picture = 0;
    pic_jpeg.coded_buf = va->mCodedBuf;
    status = vaCreateBuffer(va->mDpy, va->mContextId, VAEncPictureParameterBufferType,
                            sizeof(pic_jpeg), 1, &pic_jpeg, &(va->mPicParamBuf));
    CHECK_STATUS(status, "vaCreateBuffer", __LINE__)

    status = setJpegQuality(out.quality);
    if (status)
        goto exit;

    status = startJpegEncoding(aSurface);
    if (status)
        goto exit;

    va->mCurrentSurface = aSurface;

exit:
    return (status ? -1 : 0);
}

int JpegHwEncoder::waitToComplete(int *jpegSize)
{
    LOG1("@%s", __FUNCTION__);
    VAStatus status;
    vaJpegContext *va = mVaEncoderContext;

    if (va->mCurrentSurface == 0)
        return -1;

    status = vaSyncSurface(va->mDpy, va->mCurrentSurface);
    CHECK_STATUS(status, "vaSyncSurface", __LINE__)

    status = getJpegSize(jpegSize);

    return (status ? -1 : 0);
}

int JpegHwEncoder::getOutput(JpegCompressor::OutputBuffer &out)
{
    LOG1("@%s", __FUNCTION__);

    return getJpegData(out.buf, out.size, &out.length);
}

/****************************************************************************
 *  PRIVATE METHODS
 ****************************************************************************/


int JpegHwEncoder::setJpegQuality(int quality)
{
    LOG1("@%s, quality:%d", __FUNCTION__, quality);
    VAStatus status;
    vaJpegContext *va = mVaEncoderContext;
    unsigned char *luma_matrix = va->mQMatrix.lum_quantiser_matrix;
    unsigned char *chroma_matrix = va->mQMatrix.chroma_quantiser_matrix;

    // the below are optimal quantization steps for JPEG encoder
    // Those values are provided by JPEG standard
    va->mQMatrix.load_lum_quantiser_matrix = 1;
    va->mQMatrix.load_chroma_quantiser_matrix = 1;

static const unsigned char StandardQuantLuma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99
};
static const unsigned char StandardQuantChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

    uint32_t uc_qVal;
    uint32_t uc_j;
    unsigned short ui16_q_factor;
    // Only use 2 tables
    // set the quality to the same as libjpeg, libjpeg support from 1 to 100
    ui16_q_factor = CLIP(quality, 100, 1); // set the quality to [1 to 100]

    // This formula is provided by IJG which is the owner of the libjpeg.
    // JPEG standard doesn't have the concept "quality" at all.
    // Every encoder can design their own quality formula,
    // But most of them follow libjpeg's formula, a widely accepted one.
    ui16_q_factor = (ui16_q_factor < 50) ? (5000 / ui16_q_factor) : (200 - ui16_q_factor * 2);
    for(uc_j = 0; uc_j < 64; uc_j++) {
        uc_qVal = (StandardQuantLuma[uc_j] * ui16_q_factor + 50) / 100;
        luma_matrix[uc_j] = (unsigned char)CLIP(uc_qVal, 255, 1);
    }
    for(uc_j = 0; uc_j < 64; uc_j++) {
        uc_qVal = (StandardQuantChroma[uc_j] * ui16_q_factor + 50) / 100;
        chroma_matrix[uc_j] = (unsigned char)CLIP(uc_qVal, 255, 1);
    }

    status = vaCreateBuffer(va->mDpy, va->mContextId, VAQMatrixBufferType,
                sizeof(va->mQMatrix), 1, &va->mQMatrix, &va->mQMatrixBuf);

    return 0;
}

int JpegHwEncoder::startJpegEncoding(VASurfaceID aSurface)
{
    LOG1("@%s", __FUNCTION__);
    VAStatus status;
    vaJpegContext *va = mVaEncoderContext;

    status = vaBeginPicture(va->mDpy, va->mContextId, aSurface);
    CHECK_STATUS(status, "vaBeginPicture", __LINE__)

    status = vaRenderPicture(va->mDpy, va->mContextId, &va->mQMatrixBuf, 1);
    CHECK_STATUS(status, "vaRenderPicture", __LINE__)

    status = vaRenderPicture(va->mDpy, va->mContextId, &va->mPicParamBuf, 1);
    CHECK_STATUS(status, "vaRenderPicture", __LINE__)

    status = vaEndPicture(va->mDpy, va->mContextId);
    CHECK_STATUS(status, "vaEndPicture", __LINE__)

    return 0;
}

int JpegHwEncoder::getJpegSize(int *jpegSize)
{
    LOG1("@%s", __FUNCTION__);
    VAStatus status;
    VACodedBufferSegment *buf_list = NULL;
    vaJpegContext *va = mVaEncoderContext;

    if (NULL == jpegSize) {
        LOGE("@%s, line:%d, jpegSize:%p", __FUNCTION__, __LINE__, jpegSize);
        return -1;
    }
    *jpegSize = 0;
    status = vaMapBuffer(va->mDpy, va->mCodedBuf, (void **)&(va->mCodedBufList));
    CHECK_STATUS(status, "vaMapBuffer", __LINE__)

    buf_list = va->mCodedBufList;

    while (buf_list != NULL) {
        *jpegSize += buf_list->size;
        buf_list = (VACodedBufferSegment *)buf_list->next;
    }

    LOG1("@%s, jpeg size:%d", __FUNCTION__, *jpegSize);

    // We will unmap the mCodedBuf when we read the data in getOutput()
    return 0;
}


/**
 * Copies the JPEG bitstream to the user provided buffer
 *
 * It copies the JPEG bitstream from the VA buffer (mCodedBuf)
 * to the user provided buffer dstPtr
 *
 * It also returns the size of the actual JPEG bitstream
 *
 * \param pdst [in] pointer to the user provided
 * \param dstSize [in] size of the user provided buffer
 * \param jpegSize [out] pointer to the int that stores the actual size
 * of the JPEG bitstream
 *
 * \return 0 for success
 * \return -1 for failure
 */
int JpegHwEncoder::getJpegData(void *dstPtr, int dstSize, int *jpegSize)
{
    LOG1("@%s", __FUNCTION__);
    VAStatus status;
    VACodedBufferSegment *bufferList = NULL;
    vaJpegContext *va = mVaEncoderContext;
    int segmentSize = 0;
    int writtenSize = 0;
    unsigned char *p = (unsigned char *)dstPtr;
    unsigned char *src;

    if (NULL == dstPtr || NULL == jpegSize) {
        LOGE("@%s, line:%d, dstPtr:%p, jpegSize:%p",
              __FUNCTION__, __LINE__, dstPtr, jpegSize);
        return -1;
    }

    if(va->mCodedBufList == NULL) {
        status = vaMapBuffer(va->mDpy, va->mCodedBuf, (void **)&va->mCodedBufList);
        CHECK_STATUS(status, "vaMapBuffer", __LINE__)
    }

    bufferList = va->mCodedBufList;

    while (bufferList != NULL) {
        src = (unsigned char *) bufferList->buf;
        segmentSize = bufferList->size;
        writtenSize += segmentSize;

        if (writtenSize > dstSize) {
            LOGE("@%s, line:%d, generated JPEG size(%d) is too big > provided buffer(%d)",
                 __FUNCTION__, __LINE__, writtenSize, dstSize);
            return -1;
        }
        memcpy(p, src, segmentSize);

        p +=  segmentSize;
        bufferList = (VACodedBufferSegment *)bufferList->next;
    }

    *jpegSize = writtenSize;
    LOG1("@%s, line:%d, jpeg size:%d", __FUNCTION__, __LINE__, writtenSize);

    status = vaUnmapBuffer(va->mDpy, va->mCodedBuf);
    CHECK_STATUS(status, "vaUnmapBuffer", __LINE__)
    va->mCodedBufList = NULL;

    return 0;
}

void JpegHwEncoder::destroySurfaces(void)
{
    LOG1("@%s", __FUNCTION__);
    vaJpegContext *va = mVaEncoderContext;
    if (va->mDpy && va->mContextId)
        vaDestroyContext(va->mDpy, va->mContextId);
    if (va->mDpy && va->mSurfaceIds)
        vaDestroySurfaces(va->mDpy, va->mSurfaceIds, 1);

    va->mBuff2SurfId.clear();
}



}

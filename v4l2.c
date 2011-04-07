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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/atomisp.h>
#include "v4l2.h"

#ifndef LOG_TAG
#define LOG_TAG "V4L2"
#endif

#define CLEAR(x) memset (&(x), 0, sizeof (x))

static char *dev_name_array[2] = {"/dev/video0", "/dev/video1"};

int v4l2_capture_open(int device)
{
    int fd;
    struct stat st;

    if ((device < V4L2_FIRST_DEVICE) || (device > V4L2_SECOND_DEVICE)) {
        LOGE("ERR(%s): Wrong device node %d\n", __func__, device);
        return -1;
    }

    char *dev_name = dev_name_array[device];
    LOG1("---Open video device %s---", dev_name);

    if (stat (dev_name, &st) == -1) {
        LOGE("ERR(%s): Error stat video device %s: %s", __func__,
             dev_name, strerror(errno));
        return -1;
    }

    if (!S_ISCHR (st.st_mode)) {
        LOGE("ERR(%s): %s not a device", __func__, dev_name);
        return -1;
    }

    fd = open(dev_name, O_RDWR);

    if (fd <= 0) {
        LOGE("ERR(%s): Error opening video device %s: %s", __func__,
             dev_name, strerror(errno));
        return -1;
    }

    return fd;
}

void v4l2_capture_close(int fd)
{
    /* close video device */
    LOG1("----close device ---");
    if (fd < 0) {
        LOGW("W(%s): Not opened\n", __func__);
        return ;
    }

    if (close(fd) < 0) {
        LOGE("ERR(%s): Close video device failed!", __func__);
        return;
    }
}

int v4l2_capture_querycap(int fd, struct v4l2_capability *cap)
{
    int ret = 0;

    ret = ioctl(fd, VIDIOC_QUERYCAP, cap);

    if (ret < 0) {
        LOGE("ERR(%s): :VIDIOC_QUERYCAP failed\n", __func__);
        return ret;
    }

    if (!(cap->capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOGE("ERR(%s):  no capture devices\n", __func__);
        return -1;
    }

    if (!(cap->capabilities & V4L2_CAP_STREAMING)) {
        LOGE("ERR(%s): is no video streaming device", __func__);
        return -1;
    }

    LOG1( "driver:      '%s'", cap->driver);
    LOG1( "card:        '%s'", cap->card);
    LOG1( "bus_info:      '%s'", cap->bus_info);
    LOG1( "version:      %x", cap->version);
    LOG1( "capabilities:      %x", cap->capabilities);

    return ret;
}

int v4l2_capture_s_input(int fd, int index)
{
    struct v4l2_input input;
    int ret;

    LOG1("VIDIOC_S_INPUT");
    input.index = index;

    ret = ioctl(fd, VIDIOC_S_INPUT, &input);

    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_S_INPUT index %d failed\n", __func__,
             input.index);
        return ret;
    }
    return ret;
}

int v4l2_capture_s_format(int fd, int w, int h, int fourcc)
{
    int ret;
    struct v4l2_format v4l2_fmt;
    CLEAR(v4l2_fmt);
    LOG1("VIDIOC_S_FMT");

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ret = ioctl (fd,  VIDIOC_G_FMT, &v4l2_fmt);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_G_FMT failed %s\n", __func__, strerror(errno));
        return -1;
    }

    v4l2_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_fmt.fmt.pix.width = w;
    v4l2_fmt.fmt.pix.height = h;
    v4l2_fmt.fmt.pix.pixelformat = fourcc;
    v4l2_fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    ret = ioctl(fd, VIDIOC_S_FMT, &v4l2_fmt);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_S_FMT failed %s\n", __func__, strerror(errno));
        return -1;
    }
    return 0;
}

int v4l2_capture_request_buffers(int fd, uint num_buffers)
{
    struct v4l2_requestbuffers req_buf;
    int ret;
    CLEAR(req_buf);

    if (memory_userptr)
        req_buf.memory = V4L2_MEMORY_USERPTR;
    else
        req_buf.memory = V4L2_MEMORY_MMAP;
    req_buf.count = num_buffers;
    req_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    LOG1("VIDIOC_REQBUFS, count=%d", req_buf.count);
    ret = ioctl(fd, VIDIOC_REQBUFS, &req_buf);

    if (ret < 0) {
        LOGE("ERR(%s): VIDIOC_REQBUFS %d failed %s\n", __func__,
             num_buffers, strerror(errno));
        return ret;
    }

    if (req_buf.count < num_buffers)
        LOGW("W(%s)Got buffers is less than request\n", __func__);

    return req_buf.count;
}

/* MMAP the buffer or allocate the userptr */
int v4l2_capture_new_buffer(int fd, int index, struct v4l2_buffer_info *buf)
{
    void *data;
    int ret;
    struct v4l2_buffer *vbuf = &buf->vbuffer;

    LOG1("%s\n", __func__);

    /* FIXME: Add the userptr here */
    if (memory_userptr)
        vbuf->memory = V4L2_MEMORY_USERPTR;
    else
        vbuf->memory = V4L2_MEMORY_MMAP;

    vbuf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf->index = index;
    ret = ioctl(fd , VIDIOC_QUERYBUF, vbuf);

    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_QUERYBUF failed %s\n", __func__, strerror(errno));
        return ret;
    }

    if (memory_userptr) {
         vbuf->m.userptr = (unsigned int)(buf->data);
    } else {
        data = mmap(NULL, vbuf->length, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    vbuf->m.offset);

        if (MAP_FAILED == data) {
            LOGE("ERR(%s):mmap failed %s\n", __func__, strerror(errno));
            return -1;
        }
        buf->data = data;
    }

    buf->length = vbuf->length;
    LOG2("%s: index %u\n", __func__, vbuf->index);
    LOG2("%s: type %d\n", __func__, vbuf->type);
    LOG2("%s: bytesused %u\n", __func__, vbuf->bytesused);
    LOG2("%s: flags %08x\n", __func__, vbuf->flags);
    LOG2("%s: memory %u\n", __func__, vbuf->memory);
    if (memory_userptr) {
        LOG1("%s: userptr:  %lu", __func__, vbuf->m.userptr);
    }
    else {
        LOG1("%s: MMAP offset:  %u", __func__, vbuf->m.offset);
    }

    LOG2("%s: length %u\n", __func__, vbuf->length);
    LOG2("%s: input %u\n", __func__, vbuf->input);

    return ret;
}

/* Unmap the buffer or free the userptr */
int v4l2_capture_free_buffer(int fd, struct v4l2_buffer_info *buf_info)
{
    int ret = 0;
    void *addr = buf_info->data;
    size_t length = buf_info->length;

    LOG1("%s: munmap\n", __func__);

    if (!memory_userptr) {
        if ((ret = munmap(addr, length)) < 0) {
            LOGE("ERR(%s):munmap failed %s\n", __func__, strerror(errno));
            return ret;
        }
    }

    buf_info->data = NULL;
    buf_info->length = 0;
    buf_info->width = 0;
    buf_info->height = 0;
    buf_info->fourcc = 0;

    return ret;
}

int v4l2_capture_streamon(int fd)
{
    int ret;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    LOG1("%s\n", __func__);

    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_STREAMON failed %s\n", __func__, strerror(errno));
        return ret;
    }

    return ret;
}

int v4l2_capture_streamoff(int fd)
{
    int ret;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    LOG1("%s\n", __func__);

    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_STREAMOFF failed %s\n", __func__, strerror(errno));
        return ret;
    }

    return ret;
}

int v4l2_capture_qbuf(int fd, int index, struct v4l2_buffer_info *buf)
{
    struct v4l2_buffer *v4l2_buf = &buf->vbuffer;
    int ret;

    ret = ioctl(fd, VIDIOC_QBUF, v4l2_buf);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_QBUF index %d failed %s\n", __func__,
             index, strerror(errno));
        return ret;
    }
    LOG2("(%s): VIDIOC_QBUF finsihed", __func__);

    return ret;
}

int v4l2_capture_g_parm(int fd, struct v4l2_streamparm *parm)
{
    int ret;
    LOG1("%s\n", __func__);

    parm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fd, VIDIOC_G_PARM, parm);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_G_PARM, failed %s\n", __func__, strerror(errno));
        return ret;
    }

    LOG1("%s: timeperframe: numerator %d, denominator %d\n", __func__,
         parm->parm.capture.timeperframe.numerator,
         parm->parm.capture.timeperframe.denominator);

    return ret;
}

int v4l2_capture_s_parm(int fd, struct v4l2_streamparm *parm)
{
    int ret;

    parm->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    LOG1("%s\n", __func__);

    ret = ioctl(fd, VIDIOC_S_PARM, parm);
    if (ret < 0) {
        LOGE("ERR(%s):VIDIOC_S_PARM, failed %s\n", __func__, strerror(errno));
        return ret;
    }

    return ret;
}

int v4l2_capture_release_buffers(int fd)
{
    return v4l2_capture_request_buffers(fd, 0);
}

/* 5 seconds wait for ISP*/
#define LIBCAMERA_POLL_TIMEOUT (5 * 1000)
int v4l2_capture_dqbuf(int fd, struct v4l2_buffer *buf)
{
    int ret, i;
    int num_tries = 500;
    struct pollfd pfd[1];

    buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (memory_userptr)
        buf->memory = V4L2_MEMORY_USERPTR;
    else
        buf->memory = V4L2_MEMORY_MMAP;

    pfd[0].fd = fd;
    pfd[0].events = POLLIN | POLLERR;

    for (i = 0; i < num_tries; i++) {
        ret = poll(pfd, 1, LIBCAMERA_POLL_TIMEOUT);

        if (ret < 0 ) {
            LOGE("ERR(%s): select error in DQ\n", __func__);
            return -1;
        }

        if (ret == 0) {
            LOGE("ERR(%s): select timeout in DQ\n", __func__);
            return -1;
        }

        ret = ioctl(fd, VIDIOC_DQBUF, buf);

        if (ret >= 0)
            break;
        LOGE("DQ error -- ret is %d\n", ret);
        switch (errno) {
        case EINVAL:
            LOGE("%s: Failed to get frames from device. %s", __func__,
                 strerror(errno));
            return -1;
        case EINTR:
            LOGW("%s: Could not sync the buffer %s\n", __func__,
                 strerror(errno));
            break;
        case EAGAIN:
            LOGW("%s: No buffer in the queue %s\n", __func__,
                 strerror(errno));
            break;
        case EIO:
            break;
            /* Could ignore EIO, see spec. */

            /* fail through */
        default:
            return -1;
        }
    }

    if ( i == num_tries) {
        LOGE("ERR(%s): too many tries\n", __func__);
        return -1;
    }
    LOG2("(%s): VIDIOC_DQBUF finsihed", __func__);
    return buf->index;
}

int v4l2_register_bcd(int fd, int num_frames,
                      void **ptrs, int w, int h, int fourcc, int size)
{
    int ret = 0;
    int i;
    BC_Video_ioctl_package ioctl_package;
    bc_buf_params_t buf_param;

    buf_param.count = num_frames;
    buf_param.width = w;
    buf_param.stride = w;
    buf_param.height = h;
    buf_param.fourcc = fourcc;
    buf_param.type = BC_MEMORY_USERPTR;

    ioctl_package.ioctl_cmd = BC_Video_ioctl_request_buffers;
    ioctl_package.inputparam = (int)(&buf_param);
    ret = ioctl(fd, ATOMISP_IOC_CAMERA_BRIDGE, &ioctl_package);
    if (ret < 0) {
        LOGE("(%s): Failed to request buffers from buffer class"
             " camera driver (ret=%d).", __func__, ret);
        return -1;
    }
    LOG1("(%s): request bcd buffers count=%d, width:%d, stride:%d,"
         " height:%d, fourcc:%x", __func__, buf_param.count, buf_param.width,
         buf_param.stride, buf_param.height, buf_param.fourcc);

    bc_buf_ptr_t buf_pa;

    for (i = 0; i < num_frames; i++)
    {
        buf_pa.index = i;
        buf_pa.pa = (unsigned long)ptrs[i];
        buf_pa.size = size;
        ioctl_package.ioctl_cmd = BC_Video_ioctl_set_buffer_phyaddr;
        ioctl_package.inputparam = (int) (&buf_pa);
        ret = ioctl(fd, ATOMISP_IOC_CAMERA_BRIDGE, &ioctl_package);
        if (ret < 0) {
            LOGE("(%s): Failed to set buffer phyaddr from buffer class"
                 " camera driver (ret=%d).", __func__, ret);
            return -1;
        }
    }

    ret = ioctl(fd, ATOMISP_IOC_CAMERA_BRIDGE, &ioctl_package);
    if (ret < 0 || ioctl_package.outputparam != num_frames)
        LOGE("(%s): check bcd buffer count error", __func__);
    LOG1("(%s): check bcd buffer count = %d",
         __func__, ioctl_package.outputparam);

    return ret;
}

int v4l2_release_bcd(int fd)
{
    int ret = 0;
    BC_Video_ioctl_package ioctl_package;
    bc_buf_params_t buf_param;

    ioctl_package.ioctl_cmd = BC_Video_ioctl_release_buffer_device;
    ret = ioctl(fd, ATOMISP_IOC_CAMERA_BRIDGE, &ioctl_package);
    if (ret < 0) {
        LOGE("(%s): Failed to release buffers from buffer class camera"
             " driver (ret=%d).", __func__, ret);
        return -1;
    }

    return 0;
}

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	ControlThread.cpp \
	PreviewThread.cpp \
	PictureThread.cpp \
	VideoThread.cpp \
	AAAThread.cpp \
	AtomISP.cpp \
	DebugFrameRate.cpp \
	Callbacks.cpp \
	AtomAAA.cpp \
	AtomHAL.cpp \
	ColorConverter.cpp \
	EXIFMaker.cpp \
	JpegCompressor.cpp \
	OlaFaceDetect.cpp \

LOCAL_C_INCLUDES += \
	frameworks/base/include \
	frameworks/base/include/binder \
	frameworks/base/include/camera \
	external/jpeg \
	hardware/libhardware/include/hardware \
	external/skia/include/core \
	external/skia/include/images \
	hardware/intel/libs3cjpeg \
	$(TARGET_OUT_HEADERS)/libsharedbuffer \
	$(TARGET_OUT_HEADERS)/libmfldadvci \
	hardware/intel/libva \
	$(TARGET_OUT_HEADERS)/libCameraFaceDetection \

LOCAL_SHARED_LIBRARIES := \
	libcamera_client \
	libutils \
	libcutils \
	libbinder \
	libjpeg \
	libskia \
	libandroid \
	libui \
	libs3cjpeg \
	libsharedbuffer \
	libmfldadvci \
	libCameraFaceDetection \

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_TAGS := optional

include $(BUILD_SHARED_LIBRARY)

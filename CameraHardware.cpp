/*
**
** Copyright (C) 2009 0xlab.org - http://0xlab.org/
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

#define LOG_TAG "CameraHardware"
#include <utils/Log.h>

#include "CameraHardware.h"
#include "converter.h"
#include <fcntl.h>
#include <sys/mman.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define VIDEO_DEVICE_2        "/dev/video2"
#define VIDEO_DEVICE_0        "/dev/video0"
#define MEDIA_DEVICE        "/dev/media0"
#define PREVIEW_WIDTH        320
#define PREVIEW_HEIGHT       240
#define PIXEL_FORMAT        V4L2_PIX_FMT_YUYV

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
#endif
#define MAX_STR_LEN 35

#include <cutils/properties.h>
#ifndef UNLIKELY
#define UNLIKELY(exp) (__builtin_expect( (exp) != 0, false ))
#endif
static int mDebugFps = 0;
int version=0;
namespace android {

/* 29/12/10 : preview/picture size validation logic */
const char CameraHardware::supportedPictureSizes [] = "640x480";
const char CameraHardware::supportedPreviewSizes [] = "640x480";

const supported_resolution CameraHardware::supportedPictureRes[] = {{640, 480} , {320, 240} };

const supported_resolution CameraHardware::supportedPreviewRes[] = {{640, 480}};


CameraHardware::CameraHardware()
	: mParameters(),
	mPreviewHeap(0),
	mRawHeap(0),
	mCamera(0),
	mPreviewFrameSize(0),
	mNotifyCb(0),
	mDataCb(0),
	mDataCbTimestamp(0),
	mCallbackCookie(0),
	mMsgEnabled(0),
	previewStopped(true)

	{

		/* create camera */
		mCamera = new V4L2Camera();

		mCamera->Open_media_device(MEDIA_DEVICE);
		mCamera->Open(VIDEO_DEVICE_2);

		initDefaultParameters();

		/* whether prop "debug.camera.showfps" is enabled or not */
		char value[PROPERTY_VALUE_MAX];
		property_get("debug.camera.showfps", value, "0");
		mDebugFps = atoi(value);
		LOGD_IF(mDebugFps, "showfps enabled");

	}

void CameraHardware::initDefaultParameters()
{
	CameraParameters p;

	LOG_FUNCTION_START
	p.setPreviewSize(640, 480);
	p.setPreviewFrameRate(10);
	p.setPictureSize(640, 480);
	p.setPreviewFormat("yuv420sp");
	p.setPictureFormat("jpeg");
	p.set("jpeg-quality", "100");
	p.set("picture-size-values", CameraHardware::supportedPictureSizes);

	p.set("nightshot-mode", "0"); // off
	p.set("luma-adaptation", "0"); // FIXME: turning it on causes a crash
	p.set("antibanding", "auto"); // flicker detection and elimination
	p.set("whitebalance", "auto");
	p.set("rotation", "0");

	// List supported antibanding values
	p.set("antibanding-values",
			"off,50hz,60hz,auto");

	// List supported effects:
	p.set("effect-values",
			"none,mono,negative,solarize,sepia,posterize,whiteboard,"\
			"blackboard,aqua");

	// List supported exposure-offset:
	p.set("exposure-offset-values",
			"0,1,2,3,4,5,6,7,8,9,10");

	// List of whitebalance values
	p.set("whitebalance-values",
			"auto,incandescent,fluorescent,daylight,cloudy");

	// List of ISO values
	p.set("iso-values", "auto,high");

	if (setParameters(p) != NO_ERROR) {
		LOGE("Failed to set default parameters?!");
	}
	LOG_FUNCTION_EXIT
		return;
}
int CameraHardware::get_kernel_version()
{
	char *verstring, *dummy;
	int fd;
	int major,minor,rev,ver=-1;
	if ((verstring = (char *) malloc(MAX_STR_LEN)) == NULL )
	{
		LOGE("Failed to allocate memory\n");
		return -1;
	}
	if ((dummy = (char *) malloc(MAX_STR_LEN)) == NULL )
	{
		LOGE("Failed to allocate memory\n");
		free (verstring);
		return -1;
	}

	if ((fd = open("/proc/version", O_RDONLY)) < 0)
	{
		LOGE("Failed to open file /proc/version\n");
		goto ret;
	}

	if (read(fd, verstring, MAX_STR_LEN) < 0)
	{
		LOGE("Failed to read kernel version string from /proc/version file\n");
		close(fd);
		goto ret;
	}
	close(fd);
	if (sscanf(verstring, "%s %s %d.%d.%d%s\n", dummy, dummy, &major, &minor, &rev, dummy) != 6)
	{
		LOGE("Failed to read kernel version numbers\n");
		goto ret;
	}
	ver = KERNEL_VERSION(major, minor, rev);
ret:
	free(verstring);
	free(dummy);
	return ver;
}

CameraHardware::~CameraHardware()
{
	LOG_FUNCTION_START
		mCamera->Uninit();
	mCamera->StopStreaming();
	mCamera->Close();
	delete mCamera;
	mCamera = 0;
	LOG_FUNCTION_EXIT
}

sp<IMemoryHeap> CameraHardware::getPreviewHeap() const
{
	LOGD("Preview Heap");
	return mPreviewHeap;
}

sp<IMemoryHeap> CameraHardware::getRawHeap() const
{
	LOGD("Raw Heap");
	return mRawHeap;
}

void CameraHardware::setCallbacks(notify_callback notify_cb,
		data_callback data_cb,
		data_callback_timestamp data_cb_timestamp,
		void* user)
{
	LOG_FUNCTION_START
		Mutex::Autolock lock(mLock);
	mNotifyCb = notify_cb;
	mDataCb = data_cb;
	mDataCbTimestamp = data_cb_timestamp;
	mCallbackCookie = user;
	LOG_FUNCTION_EXIT
		return;
}

void CameraHardware::enableMsgType(int32_t msgType)
{
	LOGD("enableMsgType:%d",msgType);
	Mutex::Autolock lock(mLock);
	mMsgEnabled |= msgType;
}

void CameraHardware::disableMsgType(int32_t msgType)
{
	LOGD("disableMsgType:%d",msgType);
	Mutex::Autolock lock(mLock);
	mMsgEnabled &= ~msgType;
}

bool CameraHardware::msgTypeEnabled(int32_t msgType)
{
	LOGD("msgTypeEnabled:%d",msgType);
	Mutex::Autolock lock(mLock);
	return (mMsgEnabled & msgType);
}

bool CameraHardware::validateSize(size_t width, size_t height, const supported_resolution *supRes, size_t count)
{
	bool ret = false;
	status_t stat = NO_ERROR;
	unsigned int size;

	if ( NULL == supRes ) {
		LOGE("Invalid resolutions array passed");
		stat = -EINVAL;
	}

	if ( NO_ERROR == stat ) {
		for ( unsigned int i = 0 ; i < count; i++ ) {
			// LOGD( "Validating %d, %d and %d, %d", supRes[i].width, width, supRes[i].height, height);
			if ( ( supRes[i].width == width ) && ( supRes[i].height == height ) ) {
				ret = true;
				break;
			}
		}
	}
	return ret;
}

// ---------------------------------------------------------------------------
static void showFPS(const char *tag)
{
	static int mFrameCount = 0;
	static int mLastFrameCount = 0;
	static nsecs_t mLastFpsTime = 0;
	static float mFps = 0;
	mFrameCount++;
	if (!(mFrameCount & 0x1F)) {
		nsecs_t now = systemTime();
		nsecs_t diff = now - mLastFpsTime;
		mFps =  ((mFrameCount - mLastFrameCount) * float(s2ns(1))) / diff;
		mLastFpsTime = now;
		mLastFrameCount = mFrameCount;
		LOGD("[%s] %d Frames, %f FPS", tag, mFrameCount, mFps);
	}
}

int CameraHardware::previewThread()
{
	Mutex::Autolock lock(mPreviewLock);
	if (!previewStopped) {
		mCamera->GrabRawFrame(mRawHeap->getBase(),mRawWidth,mRawHeight);
		mRecordingLock.lock();
		if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
			mDataCbTimestamp(systemTime(), CAMERA_MSG_VIDEO_FRAME, mBuffer, mCallbackCookie);
		}
		mRecordingLock.unlock();

		if (mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
			/*yuyv422_to_yuv420sp((unsigned char *) mRawHeap->getBase(),
					(unsigned char *) mPreviewHeap->getBase(),
					mRawWidth, mRawHeight);*/
			yuyv422_to_rgb((unsigned char *) mRawHeap->getBase(),
					(unsigned char *) mPreviewHeap->getBase(),
					mRawWidth, mRawHeight);
			mDataCb(CAMERA_MSG_PREVIEW_FRAME, mRawBuffer, mCallbackCookie);
		}

		if (UNLIKELY(mDebugFps)) {
			showFPS("Preview");
		}
	}
	return NO_ERROR;
}

status_t CameraHardware::startPreview()
{

	int width, height;
	int mHeapSize = 0;
	int mRawFrameSize = 0;
	int ret = 0;

	LOG_FUNCTION_START

	if(!mCamera) {
		delete mCamera;
		mCamera = new V4L2Camera();
	}
	if (mCamera->Open(VIDEO_DEVICE_2) < 0)
		return INVALID_OPERATION;

	Mutex::Autolock lock(mPreviewLock);
	if (mPreviewThread != 0) {
		return INVALID_OPERATION;
	}

	mParameters.getPreviewSize(&mPreviewWidth, &mPreviewHeight);
	LOGD("startPreview width:%d,height:%d",mPreviewWidth,mPreviewHeight);
	if(mPreviewWidth <=0 || mPreviewHeight <=0)
	{
		LOGE("Preview size is not valid,aborting..Device can not open!!!");
		return INVALID_OPERATION;
	}
	if ((mPreviewWidth == 320) && (mPreviewHeight == 240))
	{
		LOGD("320 x 240 Preview");
		mRawWidth = 640;
		mRawHeight = 480;
	}
	else
	{
		mRawWidth = mPreviewWidth;
		mRawHeight = mPreviewHeight;
	}
	ret = mCamera->Configure(mRawWidth,mRawHeight,PIXEL_FORMAT,30);
	if(ret < 0)
	{
		LOGE("Fail to configure camera device");
		return INVALID_OPERATION;
	}

	/* clear previously buffers*/
	if(mPreviewHeap != NULL)
	{
		LOGD("mPreviewHeap Cleaning!!!!");
		mPreviewHeap.clear();
	}
	if(mRawHeap != NULL)
	{
		LOGD("mRawHeap Cleaning!!!!");
		mRawHeap.clear();
	}
	if(mHeap != NULL)
	{
		LOGD("mHeap Cleaning!!!!");
		mHeap.clear();
	}

	mPreviewFrameSize = mPreviewWidth * mPreviewHeight * 3;
	mHeapSize = (mRawWidth * mRawHeight * 3);
	mRawFrameSize = mRawWidth * mRawHeight * 3;

	mHeap = new MemoryHeapBase(mHeapSize);
	mBuffer = new MemoryBase(mHeap, 0, mHeapSize);

	mPreviewHeap = new MemoryHeapBase(mPreviewFrameSize);
	mPreviewBuffer = new MemoryBase(mPreviewHeap, 0, mPreviewFrameSize);

	mRawHeap = new MemoryHeapBase(mRawFrameSize);
	mRawBuffer = new MemoryBase(mRawHeap, 0, mRawFrameSize);

	ret = mCamera->BufferMap();
	if (ret) {
		LOGE("Camera Init fail: %s", strerror(errno));
		return UNKNOWN_ERROR;
	}

	ret = mCamera->StartStreaming();
	if (ret) {
		LOGE("Camera StartStreaming fail: %s", strerror(errno));
		return UNKNOWN_ERROR;
	}

	/* start preview thread */
	previewStopped = false;
	mPreviewThread = new PreviewThread(this);

	LOG_FUNCTION_EXIT
		return NO_ERROR;
}

void CameraHardware::stopPreview()
{
	LOG_FUNCTION_START
		sp<PreviewThread> previewThread;
	{ // scope for the lock
		Mutex::Autolock lock(mPreviewLock);
		previewStopped = true;
		previewThread = mPreviewThread;
	}

	if (mPreviewThread != 0) {
		mCamera->Uninit();
		mCamera->StopStreaming();
		mCamera->Close();
	}

	// don't hold the lock while waiting for the thread to quit
	if (previewThread != 0) {
		previewThread->requestExitAndWait();
	}

	Mutex::Autolock lock(mPreviewLock);
	mPreviewThread.clear();
	LOG_FUNCTION_EXIT
		return;
}

bool CameraHardware::previewEnabled()
{
	return mPreviewThread != 0;
}

status_t CameraHardware::startRecording()
{
	LOGE("startRecording");
	mRecordingLock.lock();
	mRecordingEnabled = true;

	mParameters.getPreviewSize(&mPreviewWidth, &mPreviewHeight);
	LOGD("getPreviewSize width:%d,height:%d",mPreviewWidth,mPreviewHeight);

	mRecordingLock.unlock();
	return NO_ERROR;

}

void CameraHardware::stopRecording()
{
	LOGE("stopRecording");
	mRecordingLock.lock();

	mRecordingEnabled = false;
	mRecordingLock.unlock();
}

bool CameraHardware::recordingEnabled()
{
	return mRecordingEnabled == true;
}

void CameraHardware::releaseRecordingFrame(const sp<IMemory>& mem)
{
	if (UNLIKELY(mDebugFps)) {
		showFPS("Recording");
	}
	return;
}

// ---------------------------------------------------------------------------

int CameraHardware::beginAutoFocusThread(void *cookie)
{
	CameraHardware *c = (CameraHardware *)cookie;
	LOGD("beginAutoFocusThread");
	return c->autoFocusThread();
}

int CameraHardware::autoFocusThread()
{
	if (mMsgEnabled & CAMERA_MSG_FOCUS)
		mNotifyCb(CAMERA_MSG_FOCUS, true, 0, mCallbackCookie);
	return NO_ERROR;
}

status_t CameraHardware::autoFocus()
{
	Mutex::Autolock lock(mLock);
	if (createThread(beginAutoFocusThread, this) == false)
		return UNKNOWN_ERROR;
	return NO_ERROR;
}

status_t CameraHardware::cancelAutoFocus()
{
	return NO_ERROR;
}

int CameraHardware::beginPictureThread(void *cookie)
{
	CameraHardware *c = (CameraHardware *)cookie;
	LOGE("begin Picture Thread");
	return c->pictureThread();
}

int CameraHardware::pictureThread()
{
	LOG_FUNCTION_START
	LOGE("Picture Thread:%d",mMsgEnabled);
	if (mMsgEnabled & CAMERA_MSG_SHUTTER) {
		// TODO: Don't notify SHUTTER for now, avoid making camera service
		// register preview surface to picture size.
		mNotifyCb(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
	}

	if (mMsgEnabled & CAMERA_MSG_RAW_IMAGE) {
		LOGD("Take Picture RAW IMAGE");
		// TODO: post a RawBuffer may be needed.
	}

	if (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE) {
		LOGD("Take Picture COMPRESSED IMAGE");
		mCamera->CreateJpegFromBuffer(mRawHeap->getBase(), mHeap->getBase());
		mDataCb(CAMERA_MSG_COMPRESSED_IMAGE, mBuffer, mCallbackCookie);
	}
	LOG_FUNCTION_EXIT
	return NO_ERROR;
}

status_t CameraHardware::takePicture()
{
	stopPreview();
	pictureThread();
	return NO_ERROR;
}

status_t CameraHardware::cancelPicture()
{
	return NO_ERROR;
}

status_t CameraHardware::dump(int fd, const Vector<String16>& args) const
{
	return NO_ERROR;
}

status_t CameraHardware::setParameters(const CameraParameters& params)
{
	Mutex::Autolock lock(mLock);
	int width  = 0;
	int height = 0;
	int framerate = 0;
	params.getPreviewSize(&width,&height);

	LOGD("Set Parameter...!! ");

	LOGD("PreviewFormat %s", params.getPreviewFormat());
	if ( params.getPreviewFormat() != NULL ) {
		if (strcmp(params.getPreviewFormat(), (const char *) CameraParameters::PIXEL_FORMAT_YUV420SP) != 0) {
			LOGE("Only yuv420sp preview is supported");
			return -EINVAL;
		}
	}

	LOGD("PictureFormat %s", params.getPictureFormat());
	if ( params.getPictureFormat() != NULL ) {
		if (strcmp(params.getPictureFormat(), (const char *) CameraParameters::PIXEL_FORMAT_JPEG) != 0) {
			LOGE("Only jpeg still pictures are supported");
			return -EINVAL;
		}
	}

	/* validate preview size */
	params.getPreviewSize(&width, &height);
	LOGD("preview width:%d,height:%d",width,height);
	if ( validateSize(width, height, supportedPreviewRes, ARRAY_SIZE(supportedPreviewRes)) == false ) {
		LOGE("Preview size not supported");
		return -EINVAL;
	}

	/* validate picture size */
	params.getPictureSize(&width, &height);
	LOGD("picture width:%d,height:%d",width,height);
	if (validateSize(width, height, supportedPictureRes, ARRAY_SIZE(supportedPictureRes)) == false ) {
		LOGE("Picture size not supported");
		return -EINVAL;
	}

	framerate = params.getPreviewFrameRate();
	LOGD("FRAMERATE %d", framerate);

	mParameters = params;

	mParameters.getPictureSize(&width, &height);
	LOGD("Picture Size by CamHAL %d x %d", width, height);

	mParameters.getPreviewSize(&width, &height);
	LOGD("Preview Resolution by CamHAL %d x %d", width, height);

	return NO_ERROR;
}

CameraParameters CameraHardware::getParameters() const
{
	CameraParameters params;

	{
		Mutex::Autolock lock(mLock);
		params = mParameters;
	}

	return params;
}

status_t CameraHardware::sendCommand(int32_t cmd, int32_t arg1, int32_t arg2)
{
	return BAD_VALUE;
}

void CameraHardware::release()
{
}

sp<CameraHardwareInterface> CameraHardware::createInstance()
{
	/*    if (singleton != 0) {
	      sp<CameraHardwareInterface> hardware = singleton.promote();
	      if (hardware != 0) {
	      return hardware;
	      }
	      }
	      sp<CameraHardwareInterface> hardware(new CameraHardware());
	      singleton = hardware;*/
	return new CameraHardware();
}

extern "C" sp<CameraHardwareInterface> openCameraHardware()
{
	return CameraHardware::createInstance();
}

}; // namespace android

#ifndef HUILANGTOUPING_NATIVE_CENTER_CROP_JPEG_H
#define HUILANGTOUPING_NATIVE_CENTER_CROP_JPEG_H

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetOutputFileDescriptor(
        JNIEnv* env,
        jobject thiz,
        jobject fileDescriptor);

JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeClearOutputFileDescriptor(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeHasOutputFileDescriptor(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativePrepareCurrentThreadForEncoding(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeBindCurrentThreadToCpu(
        JNIEnv* env,
        jobject thiz,
        jint cpuId);

JNIEXPORT jint JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetCurrentCpu(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetReservedCallbackCpu(
        JNIEnv* env,
        jobject thiz,
        jint cpuId);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetManualSplitWeights(
        JNIEnv* env,
        jobject thiz,
        jfloatArray weightsArray);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetManualCpuSplitWeights(
        JNIEnv* env,
        jobject thiz,
        jintArray cpuIdsArray,
        jfloatArray weightsArray);

JNIEXPORT void JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeClearManualSplitWeights(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetJpegSubsamplingMode(
        JNIEnv* env,
        jobject thiz,
        jint mode);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetSplitRoiQualityParams(
        JNIEnv* env,
        jobject thiz,
        jint edgeQualityReduction,
        jint edgeEncodeScalePercent);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeSetSplitLayoutParams(
        JNIEnv* env,
        jobject thiz,
        jint centerHeightPercent,
        jint centerWidthPercent,
        jint centerCoreCount,
        jboolean centerOnly,
        jint bigCoreWeightPercent);

JNIEXPORT jint JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetAvailableEncodeCpuCount(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jintArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetSuggestedEncodeCpuIds(
        JNIEnv* env,
        jobject thiz,
        jint count);

JNIEXPORT jlongArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastSendStats(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jlongArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeGetLastSplitPartStats(
        JNIEnv* env,
        jobject thiz);

JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeCenterCropRgba8888(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality);

JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeFullFrameRgba8888(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888Split2(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);


JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888Split4(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropRgba8888SplitN(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split2(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split4(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split6(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split7(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888Split8(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameRgba8888SplitN(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFrameExceptCenterRgba8888SplitN(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint centerLeft,
        jint centerTop,
        jint centerWidth,
        jint centerHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFrameWithBottomVideoRgba8888SplitN(
        JNIEnv* env,
        jobject thiz,
        jobject rgbaBuffer,
        jint bufferCapacity,
        jint srcWidth,
        jint srcHeight,
        jint rowStride,
        jint pixelStride,
        jint videoTop,
        jint videoHeight,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs,
        jint splitParts);


JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeCenterCropYuv420888(
        JNIEnv* env,
        jobject thiz,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint cropSize,
        jint jpegQuality);

JNIEXPORT jbyteArray JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeFullFrameYuv420888(
        JNIEnv* env,
        jobject thiz,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint jpegQuality);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendCenterCropYuv420888(
        JNIEnv* env,
        jobject thiz,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint cropSize,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

JNIEXPORT jboolean JNICALL
Java_com_example_huilangtoupingV3_NativeCenterCropJpegEncoder_nativeEncodeAndSendFullFrameYuv420888(
        JNIEnv* env,
        jobject thiz,
        jobject yBuffer,
        jint yCapacity,
        jobject uBuffer,
        jint uCapacity,
        jobject vBuffer,
        jint vCapacity,
        jint srcWidth,
        jint srcHeight,
        jint yRowStride,
        jint uRowStride,
        jint vRowStride,
        jint uPixelStride,
        jint vPixelStride,
        jint jpegQuality,
        jlong frameProducedNs,
        jlong callbackStartNs);

#ifdef __cplusplus
}
#endif

#endif

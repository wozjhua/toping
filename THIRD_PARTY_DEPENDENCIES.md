# 第三方依赖与本地大文件说明

本仓库不直接提交大型第三方库、DLL、SO、EXE、LIB 和本地构建缓存。整理时这些文件被移动到源码目录旁边：

```text
D:/huilang3/touping/灰狼V4源码/灰狼V4_local_binary_deps_not_uploaded/
```

该目录按原来的相对路径保存，必要时可以复制回仓库根目录。

## 需要准备的依赖

### PC 端

PC 端代码位于 `PC/`，需要：

- Visual Studio x64 Native Tools Command Prompt
- FFmpeg 8.1 full shared build
- libjpeg-turbo 64-bit
- Android NDK 27.0.12077973
- ADB 工具

原先本地依赖路径示例：

```text
huilang3/touping/ffmpeg-8.1-full_build-shared/
huilang3/touping/libjpeg-turbo64/
PC/*.dll
PC/adb.exe
```

这些文件不提交到 Git。构建前请按自己的环境下载并设置：

```bat
set NDK_ROOT=D:\SDK\ndk\27.0.12077973
set FFMPEG_DIR=D:\path\to\ffmpeg-8.1-full_build-shared
set LIBJPEG_TURBO_DIR=D:\path\to\libjpeg-turbo64
```

然后在 PC 目录中执行对应 build 脚本，例如：

```bat
call build_msvc_x64_split_no_rc.bat
```

### Android native JPEG 编码

Android native 图像编码依赖：

- Android NDK
- libjpeg-turbo source
- libjpeg-turbo Android build output

原先本地依赖路径示例：

```text
huilang3/touping/libjpeg-turbo-src/
huilang3/touping/libjpeg-turbo-build/
```

编译得到的：

```text
libhuilang_native_encoder.so
libturbojpeg.so
```

需要复制到 Android 工程：

```text
huilangtoupingV3/app/src/main/jniLibs/arm64-v8a/
```

这些 `.so` 不提交到 Git，可通过 GitHub Releases 或本地依赖包分发。

## 本次整理移出的典型文件

- `PC/avcodec-62.dll`
- `PC/avfilter-11.dll`
- `PC/avformat-62.dll`
- `PC/swscale-9.dll`
- `PC/turbojpeg.dll`
- `PC/adb.exe`
- `PC/libhuilang_native_encoder.so`
- `安卓解码/libturbojpeg.so`
- `huilangtoupingV3/app/src/main/jniLibs/arm64-v8a/*.so`
- `huilang3/touping/ffmpeg-8.1-full_build-shared/`
- `huilang3/touping/libjpeg-turbo64/`
- `huilang3/touping/libjpeg-turbo-src/`
- `huilang3/touping/libjpeg-turbo-build/`
- `huilang3/touping/libjpeg-turbo-3.1.4.1/`

## 签名文件

不要提交真实签名文件。请用 `keystore.properties.example` 作为模板，在本地创建：

```text
huilangtoupingV3/app/keystore.properties
```

并把 `.jks` 文件保存在本地。


# 第三方依赖与本地大文件说明

本仓库不直接提交大型第三方库、DLL、SO、EXE、LIB 和本地构建缓存。整理时这些文件被移动到源码目录旁边：

```text
D:/huilang3/touping/灰狼V4源码/灰狼V4_local_binary_deps_not_uploaded/
```

该目录按原来的相对路径保存，必要时可以复制回仓库根目录。

## 官方下载入口

本仓库不上传大型第三方库文件。重新搭建环境时，建议从项目官方或官方页面列出的入口下载。

### FFmpeg

- 官方下载页：https://ffmpeg.org/download.html
- 官方源码发布目录：https://ffmpeg.org/releases/
- Windows 预编译包：FFmpeg 官方下载页会列出 Windows builds，例如 gyan.dev 和 BtbN。

注意：FFmpeg 项目本身主要提供源码发布包；Windows DLL/EXE 通常来自官方页面列出的第三方构建。PC 端需要 shared build，因为运行时会用到 `avcodec-*.dll`、`avformat-*.dll`、`avutil-*.dll`、`swscale-*.dll` 等动态库。当前本地整理前使用的是 `ffmpeg-8.1-full_build-shared`，重新下载时建议选择相近版本的 `full shared` 构建。

常用 Windows 构建入口：

- gyan.dev builds：https://www.gyan.dev/ffmpeg/builds/
- BtbN FFmpeg Builds：https://github.com/BtbN/FFmpeg-Builds/releases

如果使用 gyan.dev，通常选择 release builds 里的 `ffmpeg-release-full-shared.7z`，解压后把目录路径配置为 `FFMPEG_DIR`。

### libjpeg-turbo

- 官方主页：https://libjpeg-turbo.org/
- 官方 Releases：https://github.com/libjpeg-turbo/libjpeg-turbo/releases
- 官方二进制说明：https://libjpeg-turbo.org/Documentation/OfficialBinaries
- 签名校验说明：https://libjpeg-turbo.org/Downloads/DigitalSignatures

当前项目使用 libjpeg-turbo 3.1.4.1。PC 端可使用 Windows 64-bit 官方二进制包；Android native 端建议下载官方 source tarball，然后用 Android NDK 编译出 `libturbojpeg.so`。

注意：libjpeg-turbo 的 GitHub Release 页面中，官方 source tarball 会以 `libjpeg-turbo-x.y.z.tar.gz` 形式发布；不要优先使用 GitHub 自动生成的 `Source code` 压缩包作为正式源码包。

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

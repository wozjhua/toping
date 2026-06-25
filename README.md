# 灰狼 V4 / HuiLang 投屏源码

这个仓库包含灰狼 V4 投屏相关源码，主要分为：

- `huilangtoupingV3/`：Android 投屏应用工程。
- `PC/`：Windows PC 端接收、渲染、映射编辑与输入桥接代码。
- `安卓解码/`：Android native 图像解码/裁剪相关源码。
- `huilang3/touping/`：本地第三方依赖原始放置目录，开源仓库中不包含大型第三方库文件。
- `编译命令.txt`：原始编译命令记录。

## 架构说明

项目的核心目标是极低延迟安卓投屏和 PC 映射控制。主画面链路采用 JPEG 单帧直传，用更高带宽换取更短端到端链路，避开传统视频流中的编码器队列、GOP、解码重排和播放同步缓冲。

详细架构、模块分层、数据链路和映射输入链路请看 [ARCHITECTURE.md](ARCHITECTURE.md)。

## 不随仓库上传的文件

为了避免 GitHub 仓库过大，以下内容不提交到 Git：

- FFmpeg 预编译包和 DLL。
- libjpeg-turbo 预编译包、源码包和本地 build 目录。
- Android/PC 编译产物，例如 `.so`、`.dll`、`.exe`、`.lib`、`.o`。
- Android Studio/Gradle 本地缓存。
- 本地 SDK 路径文件 `local.properties`。
- APK 签名文件和真实签名配置。

这些文件已整理到源码目录旁边的本地目录：

```text
灰狼V4_local_binary_deps_not_uploaded/
```

如果需要完整离线构建环境，请参考 [THIRD_PARTY_DEPENDENCIES.md](THIRD_PARTY_DEPENDENCIES.md)。

## 基本环境

- Windows
- Visual Studio x64 Native Tools Command Prompt
- Android NDK 27.0.12077973
- FFmpeg 8.1 shared build
- libjpeg-turbo 3.1.4.1
- Android Studio / Gradle Wrapper

## 性能与散热说明

极低延迟模式依赖高性能 Android 设备、稳定散热和未被系统温控明显降频的运行状态。推荐使用 Snapdragon 8 Elite 级别或同等级旗舰 SoC，并配合良好散热；如果设备已经因温控降频，高 FPS JPEG 编码链路的延迟和稳定性都会受到影响。

## Android 工程

Android 工程位于：

```text
huilangtoupingV3/
```

首次打开后需要让 Android Studio 重新生成本机 `local.properties`。如果需要 release 签名，请复制：

```text
huilangtoupingV3/app/keystore.properties.example
```

为：

```text
huilangtoupingV3/app/keystore.properties
```

并填写自己的 keystore 路径、alias 和密码。

## 说明

当前仓库整理为源码公开版本，大型运行依赖和编译产物请通过 Release、网盘或本地依赖目录单独分发。

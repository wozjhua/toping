# Release 混淆规则：当前用于“debug 签名 + release 混淆包”。
# 重点是保护 JNI/native 方法名，避免 R8 改名后 native 找不到符号。

# 保留 JNI native 方法名和签名。
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# 你的 native encoder 通过 System.loadLibrary + external fun 调用，类名和方法名必须稳定。
-keep class com.example.huilangtoupingV3.NativeCenterCropJpegEncoder { *; }

# Manifest 入口通常 AGP 会自动处理；这里显式保留，避免服务/Activity 被过度裁剪。
-keep class com.example.huilangtoupingV3.MainActivity { *; }
-keep class com.example.huilangtoupingV3.ScreenMirrorService { *; }

# 保留 Kotlin/Compose/反射可能用到的基础属性，减少 release-only 奇怪问题。
-keepattributes *Annotation*,InnerClasses,EnclosingMethod,Signature

# 保留泛型签名和异常信息，对调试 release 崩溃更友好。
-keepattributes Exceptions

# 如果后续开启更激进的规则，再按实际 warning 添加 dontwarn；当前先不要大面积 dontwarn。

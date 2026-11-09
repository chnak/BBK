# BBK

把 BlackBerry Q20（BB10 10.3.3）当成 PC 的**蓝牙键盘 / 鼠标**用。

手机通过 HID over Bluetooth 连到电脑，键盘、触控板、音量键、媒体键全部转成标准的
HID 报告发给 PC；不装驱动、不用配对软件，Windows 会把它识别成普通蓝牙键鼠。

## 功能

- **键盘**：QWERTY 全部按键转发；右 Shift 当 Ctrl，SYM 当 Win，黑莓键当鼠标左键
- **Alt 组合键**：Alt+字母 发送键帽上印的符号（手机自己做的字符转换，PC 不需要 Alt）
- **触控板**：单指移动、轻点左键、双击拖选、双指轻点右键、三指轻点中键、双指捏合缩放
- **物理方向键区**：Q20 触控板周围的导航键走 BPS `SCREEN_EVENT_JOYSTICK`，转成鼠标移动
- **音量 / 媒体键**：发 HID consumer 报告（静音、音量、播放、上一首/下一首）
- **屏幕常亮**：前台时把显示器和窗口标记为 KEEP_AWAKE，避免当鼠标用时熄屏
- **自动重连**：链路断了每 5 秒重连一次，PC 休眠 / USB 重新枚举后自动恢复

## 构建

需要 BB10 NDK（10.3.1 host + 10.3.1.995 target）和 Qt 4.8 的
`unsupported/blackberry-armv7le-qcc` mkspec。

```sh
export QNX_HOST=<ndk>/host_10_3_1_12/linux/x86
export QNX_TARGET=<ndk>/target_10_3_1_995/qnx6
export PATH=$QNX_HOST/usr/bin:$PATH
export QMAKESPEC=unsupported/blackberry-armv7le-qcc

qmake -spec $QMAKESPEC -o arm/Makefile BBK.pro CONFIG+=device CONFIG+=release
make -C arm -f Makefile release -j4
```

打包：

```sh
blackberry-nativepackager -devMode -package BBK.bar bar-descriptor.xml \
    -configuration Device-Release
```

## 安装

开发者模式下把 `BBK.bar` 传到设备，用 SUD 的 PPS 安装接口装（`blackberry-deploy`
或设备本机的 `bbinall.sh` 都可以）：

```sh
scp BBK.bar devuser@169.254.0.1:/accounts/1000/shared/misc/BBK.bar
ssh devuser@169.254.0.1 'sh /tmp/gdej.sh'   # 设备上的重装脚本
```

## 发布新版本

一条命令完成版本号更新、编译打包、提交推送、创建 GitHub Release 并上传 `BBK.bar`：

```powershell
.\release.ps1 -Version 1.0.1 -Notes "改了什么"
```

- `-DryRun`：只演练，不动远端
- `-SkipBuild`：复用现有 `BBK.bar`
- `-Prerelease`：标记为预发布
- `-NotesFile notes.md`：从文件读发布说明

依赖 WSL 里的 `bbndk-term50` 容器，以及凭据管理器里可用的 github.com 凭据。
`build.sh` 是脚本内部使用的编译步骤（同步源码 → 容器内 qmake/make → 打包）。

## 使用

1. 在电脑上把手机配对（蓝牙设置里会多出一个键盘/鼠标设备）
2. 打开 BBK，触控板边框变绿表示 HID 链路已建立
3. 键盘和触控板直接操作电脑；手机键盘不会弹出，也不会在本机输入

## 设备上测出来的特殊键

| 键 | Qt | nativeVirtualKey | 处理 |
|---|---|---|---|
| 左 Shift | `Qt::Key_Shift` | 0xf0e1 | 真 Shift |
| 右 Shift | `Qt::Key_Shift` | 0xf0e2 / ns 0xe5 | 当 Ctrl |
| SYM | `Qt::Key_unknown` | 0xf0d3 | 当 Win |
| 黑莓键 | `Qt::Key_Menu` | 0xf067 | 鼠标左键 |
| 返回键 | 无 Qt 事件 | — | `NAVIGATOR_SYSKEY_PRESS`，必须调 `navigator_syskey_press_response()` |

## 已知坑

- 屏幕上没有 Qt 的双击事件，双击拖选要自己计时
- 两指触摸不送 `QTouchEvent`，多点手势要从 BPS `SCREEN_EVENT_MTOUCH_*` 读
- `screen_get_context_property_pv()` 第三个参数要的是**缓冲区本身**，不是指针变量的地址
- 鼠标 HID 报告的相对位移发完必须清零

## 目录

```
main.cpp            UI、事件过滤器、BPS 屏幕/导航事件
bbkhid.h/.cpp       HID over GATT 设备、按键/鼠标/滚轮/媒体报告
BBK.pro             qmake 工程
bar-descriptor.xml  打包描述
assets/             图标
```

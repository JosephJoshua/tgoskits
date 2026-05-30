# Weston on StarryOS: 内核适配日志

## 1. 目标

使 Weston 14（Wayland 协议参考合成器）在 StarryOS 上完成三项操作:

1. **DRM 设备初始化** -- 打开 `/dev/dri/card0`，完成 KMS 资源枚举，设置显示模式，创建 framebuffer
2. **输入设备识别** -- libinput 通过 udev 发现并初始化 evdev 输入设备（键盘、鼠标、触屏），开始分发输入事件
3. **Wayland 协议服务** -- wl_display 创建监听 socket，客户端可连接并提交 surface buffer

此目标不等同于完整的桌面环境。不包含 GPU 加速渲染（仅使用 pixman 软件渲染）、热插拔处理、多显示器配置、屏幕截图、远程桌面等高级特性。目标限定于: compositor 进程正常启动，drm-backend 完成模式设置，libinput 识别至少一个输入设备，Wayland 客户端可连接并显示内容。

选择 Weston 作为适配目标，基于以下理由。图形栈是操作系统内核接口密度最高的子系统之一: 从底层的 PCI 设备枚举和 MMIO 映射，到中层的 DRM/KMS 模式设置和 dumb buffer 分配，到上层的 evdev 输入事件和 netlink 设备通知，每层都要求内核提供精确的接口行为。PostgreSQL 测试的是 POSIX 模型（进程、文件、IPC），Weston 测试的是 Linux 特有的驱动模型和用户态生态（libdrm、libinput、libudev）。两者覆盖了 StarryOS 内核接口的互补侧面。

## 2. Weston 架构与启动序列

### 2.1 整体架构

Weston 的架构可划分为四个子系统，每个子系统对内核有不同的接口要求:

```
weston (compositor 进程)
  ├─→ drm-backend          ← libdrm → /dev/dri/card0 → DRM/KMS 内核子系统
  ├─→ libinput             ← libudev → /sys, /dev/input/eventN → evdev 内核子系统
  ├─→ wayland-server       ← AF_UNIX socket → SCM_RIGHTS fd 传递
  └─→ event loop           ← epoll + timerfd
```

**drm-backend。** Weston 的显示后端，整个启动路径中内核接口密度最高的部分。初始化流程如下。

设备打开与能力协商。通过 libdrm 调用 `drmOpen("/dev/dri/card0")`，获取文件描述符后立即执行 `drmGetVersion()` 读取驱动名称、版本、日期和描述字符串。随后调用 `drmSetClientCap()` 进行能力协商: `UNIVERSAL_PLANES` 启用通用 plane 支持，`ATOMIC` 启用原子模式设置。libdrm 会盲探约十种 capability，内核必须对未知 cap 返回 0（而非报错），libdrm 以此探测内核支持的特性集。

KMS 资源枚举。`drmModeGetResources()` 返回所有 CRTC、connector、encoder 的 ID 列表。对每个 connector 调用 `drmModeGetConnector()` 获得其支持的显示模式列表（`drmModeModeInfo` 数组，包含时钟频率、水平/垂直有效像素、前后肩宽度、同步脉冲宽度等 CVT 时序参数）以及关联的 encoder。对每个 plane 调用 `drmModeGetPlane()` 获取其支持的像素格式（如 `XRGB8888`）和类型（`Primary`、`Cursor`、`Overlay`）。对每个 KMS 对象（CRTC、plane、connector）调用 `drmModeObjectGetProperties()` 获取属性列表: CRTC 的 `ACTIVE` 和 `MODE_ID`，plane 的 `FB_ID`、`CRTC_ID`、`SRC_X`、`SRC_Y`、`SRC_W`、`SRC_H`、`CRTC_X`、`CRTC_Y`、`CRTC_W`、`CRTC_H`，connector 的 `CRTC_ID`。libdrm 将这些属性映射为统一的 `drmModeProperty` 结构，上层 weston 通过属性名而非 ioctl 号访问它们。

Framebuffer 分配。创建 `drmModeCreateDumb()` 分配 dumb buffer（CPU 可访问的线性帧缓冲），获得 stride 和 handle 后调用 `drmModeAddFB2()` 将其注册为 KMS framebuffer 对象。weston 向 dumb buffer 写入像素数据（通过 pixman 软件渲染），然后通过 KMS 将 framebuffer 提交给显示硬件。

模式设置与提交。atomic 路径通过 `drmModeAtomicAlloc()` 创建原子请求，`drmModeAtomicAddProperty()` 逐条添加属性修改（CRTC 的 `ACTIVE=1` 和 `MODE_ID`，plane 的 `FB_ID`、`CRTC_ID`、源/目标矩形），最后 `drmModeAtomicCommit(DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT)` 原子提交。`ALLOW_MODESET` 标志允许此次提交涉及模式设置（首次启动时必须），`PAGE_FLIP_EVENT` 请求提交完成后内核通过 DRM 事件队列投递 `drm_event_vblank` 通知合成器可以开始渲染下一帧。Weston 使用 `drmHandleEvent()` 阻塞等待事件，`WAIT_VBLANK` 作为 fallback。

`MODE_ID` 的 blob 回环机制: 用户态通过 `drmModeCreatePropertyBlob()` 将 `drmModeModeInfo` 结构体序列化为 blob 并获取 ID，在 atomic commit 中通过 `MODE_ID` 属性引用此 blob。内核须原样保存 blob 内容，用户态后续通过 `drmModeGetPropertyBlob()` 读取时获得完全一致的字节序列。commit 完成后通过 `drmModeDestroyPropertyBlob()` 销毁。

**libinput。** Weston 的输入后端，通过 libudev 与 evdev 子系统交互。初始化流程如下。

设备发现。libinput 调用 `udev_new()` 创建 udev 上下文，`udev_enumerate_new()` 创建枚举器，`udev_enumerate_add_match_subsystem("input")` 限定子系统类型，`udev_enumerate_scan_devices()` 触发扫描。扫描过程遍历 `/sys/class/input/` 目录: 对每个条目调用 `realpath()` 解析软链接目标，再对目标路径调用 `dirname()` 获取 device 容器路径（如 `/sys/devices/virtual/input/input0`），在此路径下读取 `uevent` 文件解析设备属性。同时扫描 `/sys/dev/char/<major>:<minor>` 建立设备号到 sysfs 路径的反向映射。最后检查 `/run/udev/data/c<major>:<minor>` 文件是否存在以判断设备是否已完成 udev 初始化。

设备能力查询。libinput 打开 `/dev/input/eventN` 设备节点后，通过一组 evdev ioctl 查询设备能力: `EVIOCGBIT(EV_KEY, sizeof(keys))` 获取按键能力位图（键盘按键、鼠标按钮、触屏按钮等），`EVIOCGBIT(EV_ABS, sizeof(abs))` 获取绝对坐标轴能力位图（`ABS_X`、`ABS_Y`、`ABS_MT_POSITION_X` 等），`EVIOCGBIT(EV_REL, sizeof(rel))` 获取相对运动轴能力位图（`REL_X`、`REL_Y`、`REL_WHEEL` 等），`EVIOCGPROP(0)` 获取输入设备属性位（`INPUT_PROP_POINTER`、`INPUT_PROP_DIRECT`、`INPUT_PROP_BUTTONPAD` 等），`EVIOCGABS(ABS_X)` 等获取各绝对坐标轴的最小值、最大值、平坦区阈值和分辨率。

设备分类。libinput 根据查询结果将设备分类为鼠标、键盘、触控板、触屏、绘图板等类型。分类逻辑依赖于完整的 prop 位和 abs 轴信息: 例如，同时具有 `INPUT_PROP_POINTER` 和 `INPUT_PROP_BUTTONPAD` 的设备被判定为触控板；具有 `INPUT_PROP_DIRECT` 且支持 `ABS_X`/`ABS_Y` 的设备被判定为触屏。缺少任何一个 ioctl 的返回值，设备即无法被正确分类。

事件读取。libinput 将设备文件描述符加入 epoll 监听集合。当设备产生输入事件时（按键、移动、滚轮等），epoll 唤醒 libinput 的 dispatch 循环，libinput 通过 `read()` 读取 `struct input_event` 数组，解析后将事件分类并发送给 Weston 的处理回调。

**wayland-server。** Wayland 协议的服务端实现，基于 AF_UNIX socket 的进程间通信机制。

连接建立。Weston 创建 AF_UNIX socketpair（或 `bind` + `listen` 创建监听 socket），将 socket 文件描述符加入 epoll 监听集合。客户端通过 `WAYLAND_DISPLAY` 环境变量指定的路径连接到此 socket。

文件描述符传递。Wayland 协议的核心机制是客户端与合成器之间通过 `sendmsg(SCM_RIGHTS)` / `recvmsg(SCM_RIGHTS)` 传递文件描述符。最关键的用例是 `wl_shm` 协议: 客户端调用 `memfd_create("wayland-shm", MFD_ALLOW_SEALING)` 创建共享内存文件，通过 `ftruncate()` 分配大小，通过 `mmap(PROT_WRITE, MAP_SHARED)` 映射到自身地址空间。客户端向此 buffer 写入像素数据后，通过 Wayland 协议的 `wl_shm.create_pool(fd)` 请求将 memfd 文件描述符通过 SCM_RIGHTS 传递给合成器。合成器收到 fd 后通过 `mmap(PROT_READ, MAP_SHARED)` 映射，读取像素数据用于合成。

SCM_RIGHTS 在 SOCK_STREAM 上的语义要求文件描述符与字节流的对应关系精确: 一次 `sendmsg` 调用中，cmsg 携带的 fd 与同一调用写出的第一个字节原子交付；接收端 `recvmsg` 在读到此 fd 对应的字节范围的首字节时同时获得 fd。如果消息边界不匹配，客户端传的 fd 和合成器期望的 fd 将错位，Wayland 协议立即判定违规并断开连接。此语义在 Linux 中由 `unix_stream_sendmsg` / `unix_stream_recvmsg` 实现，通过 `scm_fp_list` 队列管理待交付的 fd。

**event loop。** Weston 使用 epoll 作为中心事件分发器，timerfd 作为定时器源。

epoll 监听集合。epoll 实例通过 `epoll_create1(EPOLL_CLOEXEC)` 创建，注册以下文件描述符: DRM 设备 fd（`EPOLLIN` 事件对应 PAGE_FLIP 完成通知）、每个 evdev 设备 fd（`EPOLLIN` 事件对应输入事件到达）、Wayland 监听 socket fd（`EPOLLIN` 事件对应新客户端连接）、每个 Wayland 客户端 fd（`EPOLLIN` 事件对应协议请求到达）、timerfd（`EPOLLIN` 事件对应定时器到期）。所有 fd 均以电平触发模式注册。

timerfd 定时器。Weston 为每帧渲染创建周期性定时器（用于帧率控制）、为动画创建一次性定时器、为输入事件创建去抖定时器。每个定时器通过 `timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)` 创建独立的 timerfd 实例，通过 `timerfd_settime()` 设置到期时间（支持相对时间和绝对时间、单次触发和周期性触发）。定时器到期后，对应的 timerfd 变为可读，epoll 唤醒 West 的主循环，Weston 通过 `read(timerfd, &expirations, sizeof(uint64_t))` 读取到期次数。

主循环。Weston 的 `wl_event_loop` 在主线程中调用 `epoll_wait()` 阻塞等待事件。被唤醒后遍历就绪事件列表，根据 fd 分发到对应的处理函数: DRM fd 就绪时调用 `drmHandleEvent()` 处理页面翻转完成事件并触发下一帧渲染；evdev fd 就绪时调用 `libinput_dispatch()` 读取输入事件并分发到焦点 surface；Wayland client fd 就绪时调用 `wl_client_connection_data()` 读取并解析协议消息；timerfd 就绪时执行定时器回调。

### 2.2 完整启动序列

以下展开 Weston 启动过程中每一步所涉及的内核接口，标注了 StarryOS 上曾发生中断的位置:

```
 0. weston --backend=drm-backend.so --tty=1
 1. drmOpen("/dev/dri/card0")                           ← 设备节点缺失
 2. drmGetVersion() → name="starry-simpledrm"
 3. drmSetClientCap(UNIVERSAL_PLANES)
 4. drmSetClientCap(ATOMIC)
 5. drmModeGetResources() → CRTCs, connectors, encoders
 6. drmModeGetConnector(conn_id) → modes
 7. drmModeGetPlaneResources() → planes
 8. drmModeGetPlane(plane_id) → format, type
 9. drmModeObjectGetProperties(CRTC_ID) → props
10. drmModeObjectGetProperties(PLANE_ID) → type=Primary
11. drmModeObjectGetProperties(CONNECTOR_ID) → CRTC_ID
12. drmModeCreatePropertyBlob(mode) → MODE_ID           ← MODE_ID 回环
13. drmModeAtomicAlloc() → atomic request
14. drmModeAtomicAddProperty(req, CRTC_ID, ACTIVE, 1)
15. drmModeAtomicAddProperty(req, CRTC_ID, MODE_ID, blob)
16. drmModeAtomicAddProperty(req, PLANE_ID, FB_ID, fb)
17. drmModeAtomicAddProperty(req, PLANE_ID, CRTC_ID, crtc)
18. drmModeAtomicAddProperty(req, PLANE_ID, SRC_X/SRC_Y/SRC_W/SRC_H, ...)
19. drmModeAtomicCommit(req, ALLOW_MODESET | PAGE_FLIP_EVENT)
20. drmHandleEvent() → PAGE_FLIP_EVENT COMPLETE
21. udev_new() → udev_enumerate_new()
22. udev_enumerate_add_match_subsystem("input")
23. udev_enumerate_scan_devices()
    ├─→ 遍历 /sys/class/input/                             ← 非软链导致失败
    │   └─→ realpath() + dirname() → 定位 device 容器
    │       └─→ 读取 uevent → ID_INPUT=1
    ├─→ 遍历 /sys/dev/char/13:64                            ← 缺失导致反查失败
    │   └─→ fstat().st_rdev → 反查 event0 syspath
    └─→ 检查 /run/udev/data/c13:64                          ← 缺失导致 skip
24. udev_device_get_parent(evdev_device) → inputN 容器
25. open("/dev/input/event0")                              ← 设备节点缺失
26. ioctl(fd, EVIOCGBIT(EV_KEY), bits) → 按键能力位
27. ioctl(fd, EVIOCGBIT(EV_ABS), bits) → 绝对轴能力位
28. ioctl(fd, EVIOCGPROP(0), &prop) → 设备属性位           ← ioctl 未实现
29. ioctl(fd, EVIOCGABS(ABS_X), &abs) → X 轴范围           ← ioctl 未实现
30. ioctl(fd, EVIOCGABS(ABS_Y), &abs) → Y 轴范围
31. epoll_ctl(epfd, EPOLL_CTL_ADD, evdev_fd, ...)
    └─→ register 无条件 wake → LT 死循环 500Hz              ← 无限循环
32. epoll_wait(epfd, events, maxevents, -1)
    └─→ 键盘按键 → IRQ → 16ms tick 兜底                    ← 延迟 16ms
33. socketpair(AF_UNIX, SOCK_STREAM) → wl_display
34. sendmsg(client_fd, SCM_RIGHTS, shm_fd)                  ← cmsg 被丢弃
35. recvmsg(client_fd, ...) → MSG_DONTWAIT 被忽略
36. timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)            ← ENOSYS
37. socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)  ← EAFNOSUPPORT
38. udev_monitor_enable_receiving(mon) → bind + recv
39. memfd_create("wayland-shm", MFD_ALLOW_SEALING)          ← 占位实现
40. fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK)                   ← seal 未实现
41. mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0)         ← 只有 PROT_WRITE
```

每一步标注了 StarryOS 上曾发生中断的位置。下文按子系统分层逐一记录修复过程。

## 3. 修复记录

### 3.1 #506 -- /dev/dri/card0: DRM/KMS 设备节点

**日期**: 2026-05-15
**链接**: https://github.com/rcore-os/tgoskits/pull/506

Weston 启动的首个阻塞点: `drmOpen("/dev/dri/card0")` 返回 `ENOENT`。StarryOS 没有 `/dev/dri/card0` 设备节点，libdrm 直接退出。

此 PR 将 simpledrm 风格的 DRM 设备节点接到 axdisplay 上，同时实现 legacy 与 atomic KMS 两条路径。legacy 路径提供 `SETCRTC` 和 `WAIT_VBLANK`，atomic 路径提供 `ATOMIC_COMMIT`、`TEST_ONLY`、`PAGE_FLIP_EVENT`。

libdrm 的行为有几个容易被忽略的细节。打开设备后会盲探一打 capability（`SET_CLIENT_CAP`），未知的 cap 必须返回 0 而不是报错。`CRTC.ACTIVE` 属性的类型必须是 `RANGE [0, 1]`，weston 的 atomic backend 硬编码检查了这个类型。如果是其他类型（比如 `ENUM` 或 `SIGNED_RANGE`），weston 直接拒绝。`MODE_ID` 通过 `CREATE/DESTROY PROPBLOB` 做 blob ID 的字节级回环: 用户态创建一个 blob 获取 ID，内核存储 blob 内容，用户态用同一个 ID 在 atomic commit 里引用，最后销毁 blob。用户态期望拿到的 MODE_ID 和传入的完全一致。

Mode list 使用 CVT-RBv1 时序参数从实际分辨率（如 1024×768）合成一条显示模式。必须用真实时序参数合成，因为用户态 mode validator 会校验水平/垂直总像素、前后肩、同步脉冲宽度等参数自洽性。随便填一组数字会被 weston 拒绝。

frame buffer 管理方面，此 PR 范围内共享 axdisplay scanout，未提供 per-buffer 的独立 `GlobalPage` 分配。这意味着所有客户端和合成器共用同一个 framebuffer。这是后续工作的明确局限性。

测试包含 3 个用例: `test-drm-version`（14 项断言，校验 driver name、version、date、description），`test-drm-modeset`（32 项断言，覆盖 CRTC/connector/encoder/plane 枚举和 mode 属性），`test-drm-atomic`（37 项断言，覆盖 atomic commit、TEST_ONLY、PAGE_FLIP_EVENT、WAIT_VBLANK）。4 架构全部 PASS。

**反思**: DRM 是 Linux 图形栈的基石，但它的内核接口规范在 Documentation/gpu/ 下有数千行文档。实现一个可用的 DRM 设备需精确理解用户态库（libdrm + weston）在每个启动阶段实际调用哪些 ioctl，以及它们对返回值的格式要求。`CRTC.ACTIVE` 必须是 `RANGE [0, 1]` 这个约束在 DRM 文档中不会明确写 weston 需要这个，只能从 weston 源码中推断。

### 3.2 #508 -- sysfs 软链、evdev minor、/run/udev seed

**日期**: 2026-05-13
**链接**: https://github.com/rcore-os/tgoskits/pull/508

Weston 的 libinput 初始化在识别输入设备时失败。此 PR 修复了三个互相依赖的问题。

**sysfs class 软链。** libudev 通过 `realpath()` + `dirname()` 定位 device 容器路径。Linux 将 `/sys/class/drm/card0` 实现为指向 `/sys/devices/virtual/drm/card0` 的软链。libudev 对 `/sys/class/drm/card0` 调 `realpath()` 得到真实路径，再调 `dirname()` 获得 `/sys/devices/virtual/drm`，在此目录下读取 `uevent` 文件获取设备属性。若 sysfs 将 class 目录实现为普通目录而非软链，`realpath()` 返回原路径不变，`dirname()` 返回 `/sys/class/drm`，该目录下无 `uevent` 文件，设备记录构建失败。StarryOS 的修复将 `/sys/class/<subsystem>/<name>` 改为指向 `/sys/devices/virtual/<subsystem>/...` 的软链。

**evdev minor 号。** Linux 的 `EVDEV_MINOR_BASE` 值为 64，定义于 `Documentation/admin-guide/devices.txt`。StarryOS 此前 sysfs 的 `dev` 文件写的是 `13:64`（major=13, minor=64），但设备节点实际创建为 `13:1`（minor=`i+1`）。libinput 通过 `fstat().st_rdev` 获取设备节点的 `(major, minor)`，在 sysfs 中扫描 `/sys/dev/char/13:64` 反查设备 syspath。两者对不上，报 "failed to create input device"。修复将 minor 号从 `i+1` 改为 `64+i`，鼠标聚合节点改为 `/dev/input/mice` 且 minor=63（与 Linux mousedev 历史值一致）。

**/run/udev/data/ 预填空文件。** libudev 的 `get_is_initialized()` 仅检查 `/run/udev/data/c<major>:<minor>` 文件是否存在。Linux 下此文件由 udevd 处理完规则后创建。StarryOS 无 udevd 运行，libinput 看到文件不存在即判定设备未初始化，直接 "skip unconfigured input device" 跳过。修复在 init.sh 中 `touch` 几个空文件（`c226:0, c29:0, c13:64..c13:71`）预填空。文件内容无关紧要，libudev 仅检查存在性。每个 `input/event<N>/uevent` 文件直接写入 `ID_INPUT=1`、`ID_INPUT_KEYBOARD=1`、`ID_INPUT_MOUSE=1` 等环境变量形式的内容，替代 udevd 的 `60-input-id.rules` 规则处理。

测试覆盖 3 个用例: `test-evdev-minor`（校验 `/dev/input/event0` 的 `(major, minor)` 为 `(13, 64)`）、`test-fallocate-memfd`（走完 weston 的 memfd_create + fallocate(mode=0) + mmap+write 路径）、`test-sysfs-class`（检查软链和 `/sys/dev/char/226:0` 存在性）。4 架构 32/32 PASS。

**反思**: 此 PR 的三个修复分别对应三种不同的 Linux 用户态契约: 文件系统路径结构（软链）、设备号约定（minor 64）、文件存在性作为布尔标志（/run/udev/data/）。三种都不对应任何一个系统调用。libinput 对 sysfs 的期望是 15 年 Linux 桌面生态演化的结果，这些期望没有集中文档，散布在 libudev/libinput 源码和 udev 规则文件中。

### 3.3 #513 -- evdev 多设备暴露与 EVIOCGPROP/EVIOCGABS

**日期**: 2026-05-13
**链接**: https://github.com/rcore-os/tgoskits/pull/513

Weston 的 libinput 初始化成功后仍然无法接收触屏事件。libinput 将触屏事件当作噪声丢弃。

两个独立问题。第一个，StarryOS 的 `input_devices()` 在发现带 `BTN_MOUSE` 位（keycode 0x110）的设备时，仅将其挂为 `/dev/input/mice`，不创建对应的 `eventN` 节点。QEMU virtio-keyboard 因为 `BTN_MISC` 在同一字节中报位，键盘也被误判为鼠标挂到 mice，后注册的设备覆盖先注册的，event0 彻底消失。修复将每个输入设备均暴露为 `/dev/input/eventN`，首个带 `BTN_MOUSE` 的设备同时 alias 到 `mice`（兼容旧 PS/2 接口）。

第二个，evdev 的 `EVIOCGPROP`（获取设备属性位）和 `EVIOCGABS`（获取绝对坐标轴范围）两个 ioctl 均未实现。libinput 通过 `EVIOCGPROP` 判断设备是 pointer（`INPUT_PROP_POINTER`）还是 touchpad（`INPUT_PROP_POINTER` + `INPUT_PROP_BUTTONPAD`），并通过 `EVIOCGABS` 获取触屏的 X/Y 轴最小/最大值和分辨率。未实现时 libinput 拿到的都是零值，对 virtio-tablet 这类绝对坐标设备报 `INPUT_PROP_POINTER=0`，将其事件当作噪声丢弃。

修复在 `axdriver_input` trait 上添加了 `get_prop_bits()` 和 `get_abs_info()` 两个默认方法，virtio-input 驱动实现这些方法将 prop 位和 `AbsInfo` 透传给 evdev。StarryOS evdev 对带相对或绝对轴但无 `INPUT_PROP_DIRECT` 的设备合成 `INPUT_PROP_POINTER` 属性。

**反思**: libinput 的设备分类逻辑依赖于能完整读取 prop 位和 abs 轴信息。缺了 `EVIOCGPROP`，libinput 无法区分鼠标、触控板和触屏。这三种设备的 evdev 事件格式本质相同（都是 `struct input_event`），区分它们的唯一依据是属性位。这是一个在实现 checklist 中容易被标记为 evdev ioctl 已实现但实际语义不完整的典型案例。

### 3.4 #509 -- Weston bringup 修复包

**日期**: 2026-05-19
**链接**: https://github.com/rcore-os/tgoskits/pull/509

此 PR 是 Wayland 路径上最复杂的单项修改，包含两组互相依赖的修复: N 组是 Weston 启动的三处阻塞修复，P 组是将之前短平快的占位实现补成完整 Linux 语义。

**N1: `sys_mmap` 的 PROT_WRITE 转换。** Weston 的 drm-pixman shadow framebuffer 在 mmap dumb buffer 时只传入 `PROT_WRITE`。StarryOS 原先仅置 W 权限位。但 RISC-V 特权规范将 `R=0 W=1` 的 leaf PTE 列为 reserved，QEMU TCG 第一次访问即触发 page fault。Linux 在所有架构上遇到 `PROT_WRITE` 均自动补 `PROT_READ`。修复跟随 Linux 行为，检测到 `PROT_WRITE` 单独出现时自动补齐 `PROT_READ`。

**N2: EventDev::register 不再无条件 wake。** 之前 epoll 注册事件时无条件调用 `waker.wake_by_ref()`。在电平触发模式下，这导致 `register → wake → consume(empty) → register → wake → ...` 的无限循环（约 500 Hz）。修复改为仅在实际存在待消费事件时才 wake，否则仅挂上 `PollSet` 等待真实数据到达。

**N3: /dev/input 设备分类修正。** 之前通过 `keys[BTN_MOUSE/8]` 单字节判断鼠标/键盘。QEMU virtio-keyboard 因 `BTN_MISC` 在同一字节中报位，导致键盘被误判为鼠标。修复改为全部走 `/dev/input/event<N>` 路径（此部分与 #513 有重叠，二者在合入时已协调）。

**P1: virtio-input IRQ 唤醒。** `VirtIoInputDev` 增加 `irq: Option<usize>` 字段，axdriver probe 时将探测到的中断号填入。`EventDev` 在 `register` 时通过 `ax_task::future::register_irq_waker` 将 epoll waker 挂载到 IRQ hook。此前键盘按键到 `epoll_wait` 醒来的延迟由 16ms 的兜底 tick 决定，挂上 IRQ 后缩短到微秒级。

**P2: register_irq_waker 重构。** 原先 IRQ hook 直接调用 `PollSet::wake`，wake 链内部使用 `SpinNoIrq` 并涉及内存分配。在 IRQ 上下文中执行可能与被抢占的任务产生死锁（单核上观测到 epoll wake 链中出现 null deref）。重构后 hook 仅置 per-IRQ pending bit 并唤醒一个常驻 drain 任务，drain 任务在普通任务上下文中调用 `PollSet::wake`，分配和 waker 链均安全。

**P3: EventDev 多事件缓冲。** `Inner.read_ahead` 从单槽位改为 `VecDeque`（容量 256）。`has_event()` 一次抽干驱动队列直至 `DevError::Again`。测试中 20 按键 burst（80 个 evdev 事件）零丢失。

**P4: AF_UNIX SOCK_STREAM 的 SCM_RIGHTS 和 MSG_DONTWAIT。** 原先 stream transport 完全忽略 `options.cmsg`，`sendmsg` 传递的文件描述符在 socketpair 中凭空消失。Weston 的 `weston-desktop-shell` 通过 `wl_shm.create_pool(fd)` 传递共享内存文件描述符时，libwayland 读不到 cmsg 直接判定协议违规。修复引入 `PendingCmsg { start_byte, end_byte, cmsg }` 队列: `send` 将一次调用写出的字节范围 `[start_byte, end_byte]` 与 cmsg 绑定；`recv` 将读取截断在下一条 cmsg 消息的 `end_byte` 处，cmsg 与对应字节范围的第一个字节原子交付。此即 Linux SOCK_STREAM 的 cmsg 语义。

`MSG_DONTWAIT` 原先被忽略，系统调用仅查看 socket 自身的 `O_NONBLOCK` 标志。修复将 `SendFlags::DONTWAIT` 和 `RecvFlags::DONTWAIT` 加入 bitflags，通过 `extra_nonblocking` 参数将临时非阻塞语义叠加到 socket 自身的阻塞模式上。同时去除了原先 `set_nonblocking(true)/(false)` flip 引入的竞态条件。

测试新增 5 个用例: `test-mmap-prot-write`、`test-evdev-event-primary`、`test-unix-scm-rights`、`test-unix-cmsg-byte-marks`、`test-dontwait`。riscv64 5/5 PASS。

**反思**: 此 PR 的复杂度在于 Weston 启动路径上的几个阻塞点是互相依赖的。mmap PROT_WRITE 修复后才能分配 framebuffer，IRQ 唤醒修复后键盘事件才能实时到达，SCM_RIGHTS 修复后 Wayland 客户端才能传递 buffer。在开发过程中，修好一个之后 West 继续往下走，在下一个阻塞点停住，这种逐步推进的方式是大型应用适配的标准模式。

### 3.5 #507 -- memfd 与 F_SEAL_*

**日期**: 2026-05-17
**链接**: https://github.com/rcore-os/tgoskits/pull/507

Wayland 的 `wl_shm` 协议要求共享内存 buffer 的大小保持稳定，依赖 memfd 的 `F_SEAL_SHRINK` 保证此约束。StarryOS 原先 `sys_memfd_create` 是一个 32 行的占位实现，仅支持 `MFD_CLOEXEC`，无 seal 概念，文件一直留在 `/tmp` 下。

此 PR 将 `sys_memfd_create` 替换为完整的 memfd 封装。Seal 语义接入三个系统调用: `F_ADD_SEALS`/`F_GET_SEALS` 接入 `sys_fcntl`；`F_SEAL_SHRINK`/`F_SEAL_GROW` 在 `sys_ftruncate` 中检查（shrink 被 `F_SEAL_SHRINK` 拒绝，grow 被 `F_SEAL_GROW` 拒绝）；`F_SEAL_WRITE` 在 `sys_mmap` 创建 `MAP_SHARED | PROT_WRITE` 映射时拒为 `EPERM`；`F_SEAL_GROW` 同时覆盖 `write(2)`，跨 EOF 写入被拒绝且文件大小回退。

并发安全设计: `add_seals` 使用 `compare_exchange` 循环原子合并 seal 位，并在持锁状态下发布。`set_len_sealed` 在持 `truncate_mtx` 状态下完成"读取长度 → 校验 seal → 修改大小"三步，关闭 `ftruncate` 与 `add_seals` 之间的 TOCTOU 窗口。

`F_SEAL_SEAL` 一旦置位，后续所有 `F_ADD_SEALS` 全部 `EPERM`。不带 `MFD_ALLOW_SEALING` 创建的 memfd 初始即设置 `F_SEAL_SEAL`，与 Linux 行为一致。memfd 命名超长（>249 字节）或含 `/` 返回 `EINVAL`。`path()` 返回 `/memfd:<name>` 格式，匹配 Linux `/proc/<pid>/fd/N` 的软链接目标。后备 tmpfs 文件路径为 `/tmp/memfd-<pid-hex>-<counter-hex>`，重试 16 次处理碰撞。`Drop` 中 best-effort `unlink` 避免 `/tmp` 膨胀。

`File::from_fd(fd)` 对 memfd 做透明处理，取出内部 `Arc<File>` 回传，使 `lseek`、`pread`、`pwrite`、`fallocate`、`sendfile` 等无需感知 memfd 包装层。

测试 `test-memfd-seals` 覆盖: 初始 `F_GET_SEALS == 0`、所有四种 seal 的独立验证、`F_SEAL_SEAL` 锁定、不带 `MFD_ALLOW_SEALING` 时的 `EPERM` 拒绝。4 架构 toml 全覆盖。

**反思**: memfd 的 seal 语义是 Linux 3.17 引入的特性，设计初衷是为 Wayland 等需要可信共享内存的 IPC 协议提供内核级保证。此 PR 的要点在于 seal 并非仅在 `fcntl` 中检查。它需要渗透到 `ftruncate`、`write`、`mmap` 三个独立入口，每个入口的检查逻辑和错误返回值不同。遗漏任何一个入口，seal 保证即为空文。

### 3.6 #512 -- AF_NETLINK 扩展

**日期**: 2026-05-14
**链接**: https://github.com/rcore-os/tgoskits/pull/512

Weston 启动 udev monitor 时，`socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT)` 返回 `EAFNOSUPPORT`。StarryOS 此前 AF_NETLINK 仅支持 `NETLINK_KOBJECT_UEVENT` 一条协议线，且仅接受 `SOCK_RAW` 类型。

此 PR 做了三件事。放开协议号与套接字类型: `NETLINK_ROUTE`、`NETLINK_GENERIC`、`NETLINK_KOBJECT_UEVENT` 三个协议号均接受，同时允许 `SOCK_DGRAM`（libnl 和 libudev 默认创建 dgram netlink socket）。

实现 per-socket 接收队列与 poll 唤醒。`NetlinkSocket` 单独持有 `Mutex<VecDeque<Vec<u8>>>` 接收队列和 `PollSet`，与 axnet-ng 的 unix-stream 使用同一套等待模型。`recvmsg` 走 `block_on(poll_io(IoEvents::IN, ...))` 标准阻塞路径，从队列弹出一条返回。非阻塞模式下空队列返回 `EAGAIN`。内核侧留有 `broadcast(protocol, msg)` 入口供后续 uevent 发射器和 rtnetlink 通知器使用。

恢复 rtnetlink 应答。`NETLINK_ROUTE` 路径的 `write()` 解析 `nlmsghdr`，对 `RTM_GETLINK` 和 `RTM_GETADDR` 合成多段响应: 每个虚拟接口对应一条 `RTM_NEWLINK` 或 `RTM_NEWADDR` 消息，填充 IFLA/IFA 属性（`IFLA_IFNAME`、`IFLA_MTU`、`IFLA_OPERSTATE`、`IFA_LOCATION`、`IFA_ADDRESS` 等），按 Linux uapi 结构组织，最后以 `NLMSG_DONE` 收尾。合成 `lo + eth0` 两个接口，足以满足 iproute2 的 `ip link show` 和 `ip addr show` 的常见探测路径。

测试覆盖 `test-netlink-rtnetlink`（校验 `RTM_GETLINK` 请求读回至少一条 `RTM_NEWLINK` 且包含 `lo` 接口名）、`test-netlink-uevent` 与 `test-netlink-genl`（socket/bind/getsockname round-trip、`SO_RCVBUF`、`O_NONBLOCK` 空队列 `EAGAIN`、`CTRL_CMD_GETFAMILY` 被内核接受）。

**反思**: netlink 是 Linux 内核态与用户态之间最重要的通信协议之一，承载了路由、设备热插拔、SELinux、audit 等十余个子系统。此 PR 实现的只是 netlink socket 层的基础设施加一个最简 rtnetlink 应答器，距离真正的 netlink 子系统还很远。但就 Weston 的启动路径而言，`lo + eth0` 两个合成接口已经足够。libnl 和 NetworkManager 的探测路径只需要验证 netlink socket 能打开、`RTM_GETLINK` 能返回结果。

### 3.7 #510 -- aarch64 EL0 cache 指令陷阱

**日期**: 2026-05-11
**链接**: https://github.com/rcore-os/tgoskits/pull/510

aarch64 架构上，用户态执行 `MRS CTR_EL0`、`DC ZVA`、`IC IVAU` 三条指令时，进程在 `__libc_start_main` 之前即收到 `SIGTRAP` 被终止。musl 和 glibc 启动早期均需读取 `CTR_EL0` 获取 cache line size，Mesa 的 SIMD 路径还需要 `DC ZVA` 进行零拷贝操作。

根因为在 `SCTLR_EL1` 寄存器中，三个 EL0 cache 指令使能位均未置位。`UCT`（bit 15）控制 EL0 对 `CTR_EL0` 的访问；`DZE`（bit 14）控制 EL0 执行 `DC ZVA`；`UCI`（bit 26）控制 EL0 执行 `DC CVAU` 和 `IC IVAU`。`init_mmu()` 此前仅设置 `M`、`C`、`I` 三位（MMU 使能、data cache 使能、instruction cache 使能），三个 EL0 使能位全为零。EL0 首次访问 cache 拓扑寄存器即触发 `EC=0x18` 同步异常。

修复将 `UCT`、`DZE`、`UCI` 三位一并置 1。此后 musl 进程可正常进入 `main`。此行为与 Linux `arch/arm64/kernel/cpufeature.c` 中 `cpu_enable_cache_maint_trap` 之外的默认 `SCTLR_EL1` 设置一致。

测试新增 `test-aarch64-cpu-feat`（用户态执行 `MRS CTR_EL0`、`DC ZVA`、`IC IVAU`，未收到 `SIGTRAP`/`SIGILL` 即通过，非 aarch64 架构跳过）和 `test-aarch64-stack-size`（校验 `RLIMIT_STACK` 至少 8 MiB，在栈上放置 6 MiB 局部数组验证不发生 SIGSEGV，非 aarch64 架构跳过）。

**反思**: EL0 cache 指令使能位是 ARM 架构中"缺了它几乎所有非平凡的用户态程序都会在启动早期崩溃"的基础设施。此问题与具体应用无关，影响的是全部 aarch64 用户态。将此 PR 列为 Weston 适配的一部分，是因为 Mesa 是第一个在 StarryOS aarch64 上执行 `DC ZVA` 的应用。

### 3.8 #511 -- GICv3 + CNTV: Apple HVF 原生执行

**日期**: 2026-05-21
**链接**: https://github.com/rcore-os/tgoskits/pull/511

StarryOS aarch64 在 Apple Silicon 上使用 `-accel hvf` 运行 QEMU 时触发 `assert(isv)`。默认 GICv2 路径需访问 GICC MMIO CPU interface，而 HVF 对部分 MMIO exception 无法提供 `ESR.ISV` 信息，QEMU 在处理 GICC 访问时断言失败。

HVF 场景下的可用路径为: GICv3 distributor/redistributor 配合 system-register CPU interface；CNTV virtual generic timer（CNTP 被 EL2 占用，EL1 访问会被 trap）；CNTV 对应 PPI 11 / IRQ 27。

此 PR 增加两套正交的 feature flag。`gic-v3` feature: `ax-plat-aarch64-peripherals` 增加 GIC backend dispatcher，启用时走 GICv3 system-register CPU interface，使用 GICD + GICR 初始化。默认继续走 GICv2 MMIO CPU interface。`cntv-timer` feature: 默认路径继续使用 CNTP / IRQ 30，启用时使用 CNTV register 并要求 `devices.timer-irq=27`。`starryos/aarch64-hvf` 作为便捷 feature 同时启用两者。`ax-plat-aarch64-qemu-virt/src/init.rs` 增加静态断言，防止 `cntv-timer + timer-irq=30` 这类错误组合通过编译。

默认 QEMU TCG 与物理板（bsta1000b、phytium-pi、raspi）保持既有 GICv2 + CNTP / PPI 14 / IRQ 30 路径不变。

测试新增 `test-aarch64-boot-smoke`（覆盖 normal 组默认 GICv2 + CNTP 路径）和 `test-aarch64-gicv3-smoke`（覆盖 opt-in `aarch64-hvf` 测试组的 GICv3 + CNTV + IRQ 27）。normal 组默认不运行 `aarch64-hvf` opt-in 测试组。

**反思**: 此 PR 是为解决一个特定的开发环境约束（macOS 无 KVM，HVF 是唯一加速选项）而引入的架构适配。Apple HVF 下的 SMP 引导和 Weston 端到端验证留作后续工作。

## 4. 回顾

### 4.1 时序

```
05-11  #510  aarch64 EL0 cache 指令        ← 架构基础设施
05-12  #504  epoll LT 重复事件              ← (与 PostgreSQL 共享)
05-13  #508  sysfs + evdev minor + udev seed ← 设备节点/sysfs
05-13  #513  evdev 多设备 + EVIOCGPROP/GABS  ← 输入子系统
05-14  #512  AF_NETLINK 扩展                 ← 协议/网络
05-15  #506  /dev/dri/card0 DRM/KMS          ← 图形栈核心
05-17  #507  memfd F_SEAL_*                  ← 图形协议
05-19  #509  Weston bringup + IRQ + cmsg     ← 综合修复包
05-21  #511  GICv3 + CNTV (Apple HVF)       ← 架构支持
```

Wayland 适配的 PR 集中在 5 月 11 日至 21 日（10 天），8 个 PR。与 PostgreSQL 适配的工作方式不同: PostgreSQL 的 PR 按 syscall 逐个推进，每个 PR 独立可合入；Wayland 的 PR 按子系统分层推进，PR 间存在依赖关系。例如 #508 (sysfs + evdev minor) 和 #513 (evdev EVIOCGPROP) 共同支撑 libinput 的完整初始化；#506 (DRM) 和 #507 (memfd) 共同支撑 Weston 的 framebuffer 分配路径。

### 4.2 缺陷分层

Wayland 适配暴露的问题可归为三个层次。

**缺失的设备节点与 ioctl**（#506, #513, #512）: `/dev/dri/card0` 完全不存在，需要从头实现 DRM/KMS 双路径。`EVIOCGPROP`/`EVIOCGABS` 完全未实现。AF_NETLINK 不支持 `NETLINK_ROUTE`。此类问题表现为 `ENOENT`、`ENOSYS`、`EAFNOSUPPORT` 等明确错误码，定位直接。

**语义偏差与隐性契约**（#508, #509, #507）: 设备节点存在但 sysfs 软链结构不对；evdev ioctl 存在但 minor 号不对齐 `EVDEV_MINOR_BASE`；memfd 可创建但 seal 语义缺失；AF_UNIX socketpair 可通信但 SCM_RIGHTS 的 byte-mark 语义未实现。此类问题不返回明确错误码。libinput 报 "skip unconfigured input device"，weston 报 "failed to create input device"。根因需要深入阅读用户态库源码才能定位。

**并发与性能**（#509 中的 IRQ waker 和 EventDev::register）: epoll LT 死循环（500 Hz）、IRQ 唤醒缺失（16ms 延迟）、IRQ 上下文中的锁竞争。此类问题不影响功能正确性（Weston 仍能启动），但严重影响可用性（500 Hz 空转导致 CPU 占用 100%，16ms 输入延迟导致交互不可用）。

### 4.3 后续工作

Weston 适配工作停止在 compositor 可启动并接收基本输入的阶段。以下若干方向须继续推进:

GPU 加速渲染。当前仅使用 pixman 软件渲染，每帧需 CPU 合成全部 surface 然后 memcpy 到 scanout buffer。GPU 加速需要 virtio-gpu 的完整 DRM 驱动支持，包括 GBM buffer 分配、PRIME fd 导出/导入、dma-buf 共享。

hotplug 处理。当前仅在启动时枚举一次 DRM 资源和输入设备，不支持运行时连接/断开显示器或 USB 输入设备。uevent netlink broadcast + udev 规则解析是前提条件。

多显示器。当前仅支持单个 connector 的单个 CRTC 模式。多显示器需要 crtc_mask、可能的 CRTC 迁移、跨 CRTC 的 atomic commit。

weston 完整测试套件。Weston 自带 `weston-test` 测试客户端和 `weston-test-runner`。运行该套件需要完整的 Wayland 协议扩展支持和可靠的输入事件注入。

### 4.4 关键结论

Weston 适配过程中的核心经验是: 内核图形栈的兼容性并非 checklist 式的功能对照。它的本质是对 Linux 用户态生态中隐性契约的逐层发现。

libudev 要求 sysfs class 目录为软链。此要求在 udev 的文档中没有记载。欲知此要求，须阅读 libudev 的 `device_new_from_syspath` 函数源码，理解它对 `realpath()` + `dirname()` 的使用方式。

libinput 要求 evdev minor 号从 64 起。此要求在 Linux `Documentation/admin-guide/devices.txt` 中有记载，但阅读这份文档的前提是已经知道 minor 号不一致是问题所在。发现路径是: libinput 报 "failed to create input device" → 阅读 libinput 源码 → 发现 `evdev_device_have_same_syspath` 函数 → 发现它通过 `fstat().st_rdev` 反查 sysfs → 发现 sysfs `dev` 文件写的是 `13:64` 而设备节点实际是 `13:1`。

libudev 的 `get_is_initialized()` 仅检查 `/run/udev/data/c<major>:<minor>` 文件存在性。此行为在 systemd 源码中实现，但 Weston 开发者不会知道此细节。在 Linux 上 udevd 已经处理好了，从来不需要手动创建这些文件。

这三项发现分别花了数小时阅读三个不同用户态库的源码。Weston 适配的难度不在于内核修改本身（所有修复加起来不过数百行），而在于定位问题需要穿透五层抽象（Weston → libinput/libdrm → libudev → sysfs/evdev → 内核），每层都可能隐藏一个微小的格式偏差。

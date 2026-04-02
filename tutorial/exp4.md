# 实验四：迁移到 OpenAMP/rpmsg 标准消息总线

## 1. 实验背景

`exp3` 已经把最小共享内存 IPC 通道提升成了“可承载服务”的私有总线：

- `exp2` 解决的是 transport：共享内存 + 双向 doorbell；
- `exp3` 解决的是 service layer：统一消息头、请求/响应、服务分发和 Linux 侧代理设备。

但 `exp3` 仍然有一个明显限制：整套链路从 transport 到服务发现，再到 Linux 侧设备入口，都是项目自定义的。

这带来两个问题：

1. 每新增一个服务，都需要自己处理一整套 Linux 侧 glue；
2. 虽然已经有“异构服务”雏形，但还没有接入 Linux 内核中已经存在的标准 AMP 消息框架。

因此，实验四的目标不是再增强私有 IPC，而是把 transport 从“自定义 ring + 自定义 service bus”迁移为：

- `OpenAMP`：远端 RTOS 侧标准框架；
- `rpmsg`：Linux 侧标准消息总线；
- `doorbell + shared memory`：继续作为底层硬件通知与共享内存载体。

换句话说，实验四要做的是：

- **保留现有硬件拓扑**
- **替换 transport 抽象**
- **把一条私有服务总线迁移成标准 AMP 消息总线**

## 2. OpenAMP 与 rpmsg 简介

### 2.1 它们分别是什么

在异构系统里，常见分工通常是：

- `Linux` 负责网络、文件系统、用户态应用、复杂业务；
- `FreeRTOS / baremetal` 负责实时控制、低延迟采样、小型服务核。

两边要协同时，通常会使用：

- 共享内存传输数据；
- 中断或 mailbox/doorbell 负责通知；
- 上层再跑一层消息框架。

在这个语境里：

- `rpmsg` 是 Linux 侧的标准消息总线；
- `OpenAMP` 是远端 RTOS/baremetal 侧常用的配套框架；
- `remoteproc` 往往负责远端生命周期管理。

本实验只做：

- `OpenAMP + rpmsg`

不做：

- `remoteproc` 远端启动/固件装载管理

因为当前板级启动方式已经固定为：

- OpenSBI domain 启动 `cpu7`
- trusted-domain 自己运行 FreeRTOS

实验四只迁移消息总线，不重做启动框架。

### 2.2 什么是 channel 和 endpoint

在 `rpmsg` 模型里，可以把两个概念这样理解：

- `channel`
  是一个逻辑服务通道，例如 `rpmsg-raw`、`qs-led`；
- `endpoint`
  是通道上的具体收发端点和回调入口。

因此，和 `exp3` 里只有一个统一的 `service dispatcher` 不同，`exp4` 里服务是通过多个 `endpoint` 自然分开的。

### 2.3 什么是 kick

`kick` 就是“通知对端去看共享队列”的动作。

它本身通常不承载业务数据，只是一个很轻的唤醒事件。

在本实验中：

- Linux 写共享区后，通过 doorbell 写 `L2R_SET`，这是一次 `kick`；
- FreeRTOS 回包后，通过 doorbell 写 `R2L_SET`，也是一次 `kick`。

也就是说，你在 `exp2/exp3` 中实现的 `doorbell`，在 `OpenAMP/rpmsg` 语境里，本质上就是底层 `notify/kick` 机制。

## 3. 实验目标

实验四的目标分成四层：

1. 在不改变现有 QEMU/PLIC/OpenSBI 硬件拓扑的前提下，引入 `OpenAMP/libmetal`；
2. 用 `rpmsg` 替代 `exp3` 的私有 transport 层；
3. 先跑通一个原始消息通道 `rpmsg-raw`；
4. 再迁移一个真实服务 `LED` 到 `rpmsg`。

实验四完成后，系统要达到下面这条链路：

- Linux 侧出现标准 `rpmsg` 设备；
- FreeRTOS 侧出现标准 `OpenAMP endpoint`；
- `rpmsg-raw` 可以完成 `ping -> pong`；
- `LED` 可以通过 `rpmsg` 被 Linux 读写。

## 4. 整体设计

### 4.1 分层结构

实验四依然沿用现有硬件资源：

- 共享内存
- doorbell
- PLIC 中断

但上层抽象发生变化。

`exp3` 的链路是：

```text
userspace
  -> /dev/quard_star_xxx_ipc
  -> Linux 私有 proxy
  -> quard_star_ipc_call()
  -> 共享内存 ring
  -> doorbell
  -> FreeRTOS ipc_task
  -> service dispatcher
  -> 具体 handler
  -> response
```

`exp4` 的链路变成：

```text
userspace
  -> /dev/rpmsg0 或 /dev/quard_star_led_rpmsg
  -> rpmsg_char / rpmsg client driver
  -> rpmsg core
  -> virtio_rpmsg_bus
  -> quard_star_rpmsg_virtio
  -> vring + shared buffer
  -> doorbell
  -> OpenAMP endpoint callback
  -> 具体 handler
  -> rpmsg response
  -> Linux rpmsg callback
  -> userspace
```

### 4.2 实验四与实验三的本质区别

实验三和实验四最关键的区别不是“有没有设备节点”，而是：

- `exp3`：你自己实现一条私有服务总线；
- `exp4`：你把服务挂到标准 `rpmsg` 总线上。

因此：

- `exp3` 中新增一个服务，通常意味着新增一个 `*_ipc` 私有代理；
- `exp4` 中新增一个服务，首先是新增一个 `rpmsg channel/endpoint`；
- Linux 是否再额外提供专用节点，取决于你要不要做服务适配。

例如本实验中：

- `/dev/rpmsg0` 是标准 `rpmsg_char` 自动暴露出来的；
- `/dev/quard_star_led_rpmsg` 是额外写的 `LED` 适配驱动，目的是让验证更直接。

## 5. 共享内存与协议布局

### 5.1 共享内存布局

实验四没有继续使用 `exp3` 的双 SPSC ring，而是改成 `OpenAMP/rpmsg` 所需的最小共享布局。

公共定义位于：

- `ipc_proto/quard_star_rpmsg.h`

核心布局如下：

```c
#define QUARD_STAR_RPMSG_SHM_SIZE      0x10000U
#define QUARD_STAR_RPMSG_RSC_OFFSET    0x0000U
#define QUARD_STAR_RPMSG_VRING0_OFFSET 0x1000U
#define QUARD_STAR_RPMSG_VRING1_OFFSET 0x3000U
#define QUARD_STAR_RPMSG_BUF_OFFSET    0x5000U
```

可以理解为：

- `0x0000 ~ 0x0fff`：resource / vdev 描述区
- `0x1000 ~ 0x2fff`：vring0
- `0x3000 ~ 0x4fff`：vring1
- `0x5000 ~ 0xffff`：buffer pool

### 5.2 服务命名

本实验先定义两个最小服务：

```c
#define QUARD_STAR_RPMSG_RAW_SERVICE "rpmsg-raw"
#define QUARD_STAR_RPMSG_LED_SERVICE "qs-led"
```

其中：

- `rpmsg-raw`
  用于验证标准消息通路；
- `qs-led`
  用于验证一个真实服务已经迁移到 `rpmsg`。

### 5.3 LED 载荷

`LED` 服务载荷很简单：

```c
struct quard_star_rpmsg_led_msg {
    u32 cmd;
    u32 led;
    u32 value;
    s32 status;
};
```

对应命令：

```c
enum quard_star_rpmsg_led_cmd {
    QUARD_STAR_RPMSG_LED_GET = 1,
    QUARD_STAR_RPMSG_LED_SET = 2,
};
```

## 6. 设备树与共享内存划分

### 6.1 为什么设备树必须重做 carveout

这是实验四最容易出问题的地方。

如果仍然让 Linux 把共享区当成普通 RAM，那么：

- `memremap()` 可能报错；
- `of_reserved_mem_device_init_by_idx()` 可能返回 `-EINVAL`；
- `shared-dma-pool` 会和 `linux,cma` 或普通内存重叠；
- `rpmsg` transport 根本起不来。

因此，实验四必须把用于 `rpmsg` 的内存区真正从 Linux 的通用内存里 carve out。

### 6.2 最终的 carveout 方案

实验四最终把顶端内存切成三部分：

1. Linux 普通内存区收窄；
2. `rpmsg` 控制区：
   - resource + vring
3. `rpmsg` buffer pool：
   - `shared-dma-pool`
4. 原有 `ipc_shm` 继续保留给 `exp3`

最终 Linux 看到的普通内存不再覆盖：

- `0xbf7e0000 ~ 0xbf7effff`：`rpmsg`
- `0xbf7f0000 ~ 0xbf7fffff`：`ipc_shm`

对应设备树修改位于：

- `dts/quard_star_uboot.dts`

## 7. FreeRTOS 侧实现

### 7.1 libmetal / OpenAMP 接入

实验四在 trusted-domain 中引入了：

- `third_party/open-amp`
- `third_party/libmetal`

同时新增了平台 glue：

- `trusted_domain/quard_star_libmetal.c`
- `trusted_domain/quard_star_openamp.c`
- `trusted_domain/quard_star_openamp.h`

其中 `quard_star_libmetal.c` 负责：

- 中断开关包装；
- cache flush/invalidate 空实现；
- 最小 machine/system hook。

### 7.2 共享区初始化

FreeRTOS 启动 OpenAMP 之前，需要先把共享区里的 resource/vring 描述初始化好：

```c
static void quard_star_openamp_init_layout(void)
{
    ...
    rsc->type = QUARD_STAR_RPMSG_RESOURCE_VDEV;
    rsc->id = QUARD_STAR_RPMSG_VIRTIO_ID_RPMSG;
    rsc->notifyid = QUARD_STAR_RPMSG_VDEV_NOTIFYID;
    rsc->dfeatures = (1U << QUARD_STAR_RPMSG_F_NS);
    ...
}
```

这一步非常关键，因为 Linux 侧 transport 需要根据这块共享描述去建立最小 virtio/rpmsg 视图。

### 7.3 OpenAMP 初始化

核心初始化流程如下：

```c
quard_star_openamp_vdev =
    rproc_virtio_create_vdev(...);

rproc_virtio_init_vring(..., 0, ...);
rproc_virtio_init_vring(..., 1, ...);

rpmsg_init_vdev(&quard_star_openamp_rvdev, ...);
```

之后再创建两个 endpoint：

```c
rpmsg_create_ept(&quard_star_raw_ept, rdev,
                 QUARD_STAR_RPMSG_RAW_SERVICE,
                 RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                 quard_star_raw_cb, NULL);

rpmsg_create_ept(&quard_star_led_ept, rdev,
                 QUARD_STAR_RPMSG_LED_SERVICE,
                 RPMSG_ADDR_ANY, RPMSG_ADDR_ANY,
                 quard_star_led_cb, NULL);
```

### 7.4 doorbell 与 OpenAMP 的衔接

FreeRTOS 侧不再自己解析 Linux 发来的私有 ring 包，而是在被 doorbell 唤醒后调用：

```c
rproc_virtio_notified(quard_star_openamp_vdev, RSC_NOTIFY_ID_ANY);
```

这一步的含义是：

- 对端 kick 过来了；
- OpenAMP 现在去检查 vring；
- 如果有新消息，就把它交给对应 endpoint callback。

### 7.5 两个最小 endpoint

`rpmsg-raw` 仍然保持最简单验证语义：

```c
if (!strncmp(rx, "ping-", 5))
    tx_len = snprintf(tx, sizeof(tx), "pong-%s", rx + 5);
else
    tx_len = snprintf(tx, sizeof(tx), "ack:%s", rx);
```

`qs-led` 则迁移为正式服务 callback：

```c
switch (msg.cmd) {
case QUARD_STAR_RPMSG_LED_GET:
    msg.value = quard_star_led_read(msg.led);
    msg.status = 0;
    break;
case QUARD_STAR_RPMSG_LED_SET:
    quard_star_led_write(msg.led, !!msg.value);
    msg.value = quard_star_led_read(msg.led);
    msg.status = 0;
    break;
default:
    msg.status = -ENOSYS;
    break;
}
```

这说明到了 `exp4`，真正变化的不是 LED 业务逻辑，而是：

- 调用入口从 `ipc_task + service dispatcher`
- 变成了 `OpenAMP endpoint callback`

## 8. Linux 侧实现

### 8.1 最小 transport glue

Linux 侧新增：

- `linux-6.1.11/drivers/rpmsg/quard_star_rpmsg_virtio.c`

它不是 `remoteproc`，而是一层最小平台 glue，用于把现有硬件资源挂到内核已有 `rpmsg` 栈上。

它主要完成四件事：

1. 映射共享控制区；
2. 绑定 buffer pool；
3. 把 doorbell 接成 `notify/kick`；
4. 构造一个 `virtio_device`，注册到 `virtio_rpmsg_bus`。

其中最核心的 notify 逻辑如下：

```c
static bool quard_star_rpmsg_virtio_notify(struct virtqueue *vq)
{
    ...
    writel(1U, qsrv->doorbell + QUARD_STAR_DOORBELL_L2R_SET);
    return true;
}
```

这意味着：

- Linux `rpmsg_send()` 的最终通知动作，仍然是已有 doorbell。

### 8.2 为什么 Linux 侧要做成虚拟 virtio transport

因为内核现有 `rpmsg` 主流实现建立在 `virtio_rpmsg_bus` 上。

所以实验四并没有重写 Linux 上层 `rpmsg`，而是选择：

- 在底部补一层 `quard_star_rpmsg_virtio`
- 让它向内核暴露一个标准 `virtio_device`

这样一来，Linux 内核已有的：

- `rpmsg_core`
- `virtio_rpmsg_bus`
- `rpmsg_char`

都可以直接复用。

### 8.3 IRQ 处理必须改成 threaded IRQ

这是本实验里一个非常关键的坑。

一开始直接在硬中断里调用：

```c
vring_interrupt(...)
```

结果 `rpmsg_ns_cb()` 在 channel 创建设备时触发了会睡眠的路径，内核报出：

- `sleeping function called from invalid context`
- `scheduling while atomic`

原因是：

- `vring_interrupt()` 最终会推进 `rpmsg` 的收包回调；
- name service 收到新 channel 后会在 Linux 内核里创建设备；
- 这条链路不能在硬中断上下文里完成。

修复方法是：

- 顶半部只清 doorbell 并返回 `IRQ_WAKE_THREAD`
- 底半部线程里调用 `vring_interrupt()`

最终实现为：

```c
static irqreturn_t quard_star_rpmsg_virtio_irq(int irq, void *data)
{
    ...
    return IRQ_WAKE_THREAD;
}

static irqreturn_t quard_star_rpmsg_virtio_irq_thread(int irq, void *data)
{
    ...
    handled |= vring_interrupt(irq, qsrv->vrings[i].vq);
    ...
}
```

### 8.4 `rpmsg_char` 与 `LED` 服务驱动

实验四最终出现了两类 Linux 接口：

1. 标准 `rpmsg_char`
   - `/dev/rpmsg0`
   - `/dev/rpmsg_ctrl0`
2. 服务适配驱动
   - `/dev/quard_star_led_rpmsg`

其中 `LED` 服务驱动位于：

- `linux-6.1.11/drivers/misc/quard_star_led_rpmsg.c`

它的逻辑是：

1. `write()` 时发 `SET`
2. `read()` 时发 `GET`
3. 等待回包
4. 检查 `cmd/status`

核心代码如下：

```c
ret = rpmsg_send(qsled->rpdev->ept, &req, sizeof(req));

wait_ret = wait_event_interruptible_timeout(qsled->readq,
                                            qsled->resp_ready,
                                            msecs_to_jiffies(1000));
```

这说明实验四已经不再需要通过私有 `quard_star_ipc_call()` 完成同步请求，而是改为：

- `rpmsg_send()`
- callback 回包
- 本地 waitqueue 同步等待

## 9. 实际数据通路

### 9.1 `rpmsg-raw`

`rpmsg-raw` 的完整路径如下：

```text
userspace
  -> /dev/rpmsg0
  -> rpmsg_char
  -> rpmsg core
  -> virtio_rpmsg_bus
  -> quard_star_rpmsg_virtio
  -> doorbell L2R
  -> OpenAMP raw endpoint
  -> pong-xxxx
  -> doorbell R2L
  -> rpmsg callback
  -> /dev/rpmsg0
```

### 9.2 `LED`

`LED` 的完整路径如下：

```text
userspace
  -> /dev/quard_star_led_rpmsg
  -> quard_star_led_rpmsg
  -> rpmsg_send()
  -> quard_star_rpmsg_virtio
  -> doorbell
  -> OpenAMP qs-led endpoint
  -> GPIO output register
  -> rpmsg response
  -> Linux rpmsg callback
  -> waitqueue wakeup
  -> userspace read()/write() 返回
```

## 10. 实验四踩到的关键坑

这一节是实验四最重要的部分之一，因为这些问题如果没有提前想清楚，`OpenAMP/rpmsg` 很容易“编过但跑不起来”。

### 10.1 `reserved-memory` 与 `linux,cma` 重叠

最开始直接在 `0xbf7e0000` 增加 `rpmsg_shm`，但 Linux 启动时报告：

- `rpmsg_shm ... overlaps with linux,cma`

原因是原本的：

- `linux,cma@b0000000`

一直覆盖到 `0xbf800000`，把 `rpmsg` 和 `ipc_shm` 都吃进去了。

修复方法：

- 收窄 `linux,cma`
- 并把顶端的共享区从 Linux 普通 RAM 中切出去

### 10.2 `rpmsg_shm` 和 `rpmsg_buf_shm` 自己重叠

一开始把：

- `rpmsg_shm` 定义成整块 `0x10000`
- `rpmsg_buf_shm` 又从 `0x5000` 开始单独切 buffer pool

结果两者天然重叠。

修复方法：

- `rpmsg_shm` 只覆盖 `resource + vring`
- `rpmsg_buf_shm` 单独覆盖 `buffer pool`

### 10.3 `shared-dma-pool` 但没有真正 carve out

只在 `reserved-memory` 里写节点还不够。

如果 Linux 仍然把这段地址当普通 RAM，那么：

- `of_reserved_mem_device_init_by_idx()` 内部仍会失败；
- 最终 probe 返回 `-EINVAL`。

实验四最后采用的修复方式是：

1. 调整 `memory@80000000` 的 `reg`，让 Linux 普通内存不再覆盖顶端共享区；
2. `rpmsg_buf_shm` 使用 `shared-dma-pool + no-map`；
3. Linux transport 通过 `of_reserved_mem_device_init_by_idx()` 绑定 buffer pool。

### 10.4 在硬中断里直接推进 `vring_interrupt()`

这会触发：

- `sleeping function called from invalid context`
- `scheduling while atomic`

原因是 `rpmsg` 的 channel 创建设备路径可能睡眠。

修复方法：

- 改成 threaded IRQ

### 10.5 与 `exp3` 共用同一个 IRQ

实验四不是替换 `exp3`，而是和 `exp3` 并存。

因此 Linux 侧 IRQ 61 同时被：

- `quard_star_ipc`
- `quard_star_rpmsg_virtio`

共享。

这又引出两个小坑：

1. `exp3` 原驱动必须改为 `IRQF_SHARED`
2. `exp4` 这边如果加了 `IRQF_ONESHOT`，会和旧驱动 flags 不匹配

最终修复方法：

- 双方都用可兼容的共享 IRQ flags；
- `exp4` 通过 `devm_request_threaded_irq()` 运行底半部；
- 不使用和旧驱动冲突的 `IRQF_ONESHOT`。

### 10.6 FreeRTOS 侧 doorbell 监听者不再只有一个

`exp3` 时 FreeRTOS 只有一个 `ipc_task` 等 doorbell。

但到 `exp4`，同一个 doorbell 还需要唤醒：

- `ipc_task`
- `openamp_task`

因此 `trusted_domain/doorbell.c` 必须从：

- 单一 task handle

改成：

- 可注册多个 task handle 并广播通知

否则 `exp3` 和 `exp4` 无法共存。

## 11. 构建步骤

实验四最终涉及：

- trusted-domain
- kernel
- uboot dtb
- firmware
- rootfs bootfs

构建顺序如下：

```bash
./build.sh trusted_domain
./build.sh kernel
./build.sh uboot_dtb
./build.sh firmware
./build.sh rootfs bootfs
```

运行：

```bash
./run.sh customize2
```

或者：

```bash
./run.sh customize4
```

## 12. 验证方法

### 12.1 检查 rpmsg 是否上线

Linux 启动后查看：

```sh
dmesg | grep -i -E 'quard_star_rpmsg|rpmsg host|creating channel'
```

预期看到：

```text
quard_star_rpmsg_virtio quard_star_rpmsg: Quard Star rpmsg virtio ready ...
virtio_rpmsg_bus virtio6: rpmsg host is online
virtio_rpmsg_bus virtio6: creating channel rpmsg-raw addr 0x400
virtio_rpmsg_bus virtio6: creating channel qs-led addr 0x401
```

### 12.2 检查设备节点

```sh
ls -l /dev/rpmsg* /dev/quard_star_led_rpmsg
```

预期至少看到：

```text
/dev/rpmsg0
/dev/rpmsg_ctrl0
/dev/quard_star_led_rpmsg
```

### 12.3 验证 `rpmsg-raw`

```sh
printf 'ping-0001' > /dev/rpmsg0
head -c 9 /dev/rpmsg0; echo
```

预期：

```text
pong-0001
```

### 12.4 验证 `LED`

关闭：

```sh
printf '\x00\x00\x00\x00' > /dev/quard_star_led_rpmsg
head -c 4 /dev/quard_star_led_rpmsg | od -An -tu4
```

预期：

```text
0
```

打开：

```sh
printf '\x01\x00\x00\x00' > /dev/quard_star_led_rpmsg
head -c 4 /dev/quard_star_led_rpmsg | od -An -tu4
```

预期：

```text
1
```

## 13. 实验总结

实验四完成后，系统已经从：

- `exp3` 的私有 IPC 服务总线

迁移出一条新的：

- `OpenAMP/rpmsg` 标准消息总线

并且已经验证了两件关键事情：

1. `doorbell + shared memory` 这组底层硬件资源完全可以承载标准 AMP 消息框架；
2. 真实服务可以不再依赖私有 `*_ipc` transport，而是迁移到标准 `rpmsg endpoint/channel` 上。

这意味着后续继续扩展时，重点不再是“如何继续造自己的总线”，而是：

- 如何把新服务组织成 `rpmsg endpoint`
- 如何决定 Linux 侧暴露成 `rpmsg_char` 还是标准子系统适配

例如后续如果要做 I2C/IMU，就可以考虑两条路线：

1. 先做一个 `qs-i2c` 或 `qs-imu` 的最小 rpmsg 服务；
2. 再决定是否把它进一步接成 Linux 标准 `i2c_adapter` 或 `IIO` 设备。

这就是实验四相对实验三最大的价值：

- `exp3` 证明了“私有服务层能工作”；
- `exp4` 证明了“标准 AMP 消息总线已经打通”。

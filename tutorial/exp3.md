# 实验三：在最小 IPC 上构建服务层

## 1. 实验背景

`exp2` 已经完成了一条最小但完整的异构核通信链：

- 数据面使用共享内存 ring buffer；
- 通知面使用双向 doorbell 中断；
- Linux 和 FreeRTOS 都可以被对端事件唤醒；
- 最终已经能够完成 `ping -> pong` 的双向阻塞收发。

但 `exp2` 解决的仍然只是 transport 层问题。此时 IPC 通道虽然已经能收发消息，却还不能很好地承载“真实服务”：

- 所有 payload 都只是原始字节流；
- FreeRTOS 侧没有正式的服务分发器；
- Linux 侧没有统一的请求封装接口；
- 请求、响应、错误和超时都没有被抽象为稳定的服务语义。

因此，实验三的目标不是继续增强 doorbell 或 ring 本身，而是在 `exp2` 的 transport 之上增加一层最小服务层，使这条链路可以真正挂接外设和远端服务。

## 2. 实验目标

实验三分为两个阶段：

### 2.1 `exp3-1`：把最小 IPC 通道升级为最小服务通道

目标如下：

1. 在 ring payload 中增加统一消息头；
2. 保留旧版字符串 `ping/pong` 路径，避免破坏 `exp2` 的已有验证方法；
3. 在 FreeRTOS 侧实现正式的 `service/opcode` 分发；
4. 选两个最小服务接入 IPC：
   - `demochar`
   - `LED`
5. 在 Linux 侧新增对应代理设备，验证用户空间可以通过 IPC 使用这些服务。

### 2.2 `exp3-2`：把 Linux 侧服务调用路径收敛为统一接口

目标如下：

1. 提供统一的 Linux 侧请求封装 API；
2. 增加请求级超时；
3. 增加 `seq` 匹配与坏包检测；
4. 把这些公共逻辑从各个 `*_ipc` 驱动中抽走；
5. 为后续迁移到 `OpenAMP/rpmsg` 做接口层准备。

实验三完成后，系统就不再只是“能传一条消息”，而是已经具备“承载正式服务”的最小能力。

## 3. 整体设计

### 3.1 分层思路

实验三仍然沿用 `exp2` 的 transport：

1. 共享内存 ring buffer 负责传输数据；
2. doorbell 负责通知对端；
3. Linux 和 FreeRTOS 双方都继续通过中断唤醒。

新增的只是服务层：

1. payload 从“裸字符串”升级为“消息头 + 业务载荷”；
2. FreeRTOS 在 `ipc_task` 内部先解析协议头，再分发到具体服务；
3. Linux 不再把 `/dev/quard_star_ipc` 当作最终接口，而是在其上构建服务代理设备。

整个调用链如下：

```text
userspace
   -> /dev/quard_star_demochar_ipc 或 /dev/quard_star_led_ipc
   -> Linux IPC client API
   -> quard_star_ipc core
   -> shared memory + doorbell
   -> FreeRTOS ipc_task
   -> service dispatcher
   -> demochar / gpio
   -> response
   -> Linux userspace
```

### 3.2 为什么保留 legacy ping/pong

实验三并没有一开始就彻底废弃 `exp2` 的字符串路径，而是选择“服务协议”和“legacy 字符串”共存。

这样做有两个目的：

1. `exp2` 的验证命令仍然有效，可以快速判断 transport 是否回归；
2. 新增服务层时，如果出现问题，可以迅速区分“transport 坏了”还是“service layer 坏了”。

因此，实验三中的 FreeRTOS `ipc_task` 实际上有两条路径：

- 如果收到的是正式服务包，则进入服务分发逻辑；
- 如果收到的不是正式服务包，则继续按 `ping/pong` 方式处理。

## 4. 协议设计

### 4.1 统一消息头

公共协议头定义在 `ipc_proto/quard_star_ipc.h`：

```c
struct quard_star_ipc_msg_hdr {
    unsigned int magic;
    unsigned char version;
    unsigned char type;
    unsigned char service;
    unsigned char opcode;
    unsigned int seq;
    int status;
    unsigned int len;
};
```

字段含义如下：

- `magic`
  用于识别这是不是正式服务包；
- `version`
  用于协议版本控制；
- `type`
  区分 `REQ/RESP/EVT`；
- `service`
  区分不同服务；
- `opcode`
  区分同一服务内部的具体命令；
- `seq`
  用于请求和响应匹配；
- `status`
  用于返回业务错误码；
- `len`
  表示 payload 长度。

### 4.2 当前定义的服务

实验三先定义了两个最小服务：

```c
#define QUARD_STAR_IPC_SERVICE_DEMOCHAR  1U
#define QUARD_STAR_IPC_SERVICE_LED       2U
```

对应 opcode 定义如下：

```c
#define QUARD_STAR_IPC_DEMOCHAR_READ     1U
#define QUARD_STAR_IPC_DEMOCHAR_WRITE    2U
#define QUARD_STAR_IPC_DEMOCHAR_VERSION  3U

#define QUARD_STAR_IPC_LED_GET           1U
#define QUARD_STAR_IPC_LED_SET           2U
```

同时定义了最小状态码：

```c
#define QUARD_STAR_IPC_STATUS_OK       0
#define QUARD_STAR_IPC_STATUS_INVAL   -22
#define QUARD_STAR_IPC_STATUS_NOSYS   -38
#define QUARD_STAR_IPC_STATUS_IO      -5
#define QUARD_STAR_IPC_STATUS_TOOLONG -90
```

### 4.3 业务载荷结构

当前两个服务都只需要很小的 payload：

```c
struct quard_star_ipc_demochar_value {
    unsigned int value;
};

struct quard_star_ipc_led_state {
    unsigned int led;
    unsigned int value;
};
```

这里刻意保持简单，原因是实验三要验证的是“服务层抽象是否跑通”，不是追求复杂协议。

## 5. FreeRTOS 侧实现

FreeRTOS 侧核心逻辑位于 `trusted_domain/main.c`。

### 5.1 服务包解析与回包

服务包的解析入口是：

- `ipc_parse_header()`
- `ipc_handle_service_request()`
- `ipc_send_response()`

其中 `ipc_send_response()` 负责统一构造响应：

```c
static int ipc_send_response(unsigned int seq, unsigned char service,
                             unsigned char opcode, int status,
                             const void *payload, unsigned int payload_len)
{
    struct quard_star_ipc_msg_hdr hdr;

    hdr.magic = QUARD_STAR_IPC_MSG_MAGIC;
    hdr.version = QUARD_STAR_IPC_MSG_VERSION;
    hdr.type = QUARD_STAR_IPC_MSG_TYPE_RESP;
    hdr.service = service;
    hdr.opcode = opcode;
    hdr.seq = seq;
    hdr.status = status;
    hdr.len = payload_len;
    ...
}
```

这样一来，FreeRTOS 侧所有服务都复用同一套 response 格式，而不需要各自拼接返回包。

### 5.2 服务分发器

正式服务分发逻辑如下：

```c
switch (hdr.service) {
case QUARD_STAR_IPC_SERVICE_DEMOCHAR:
    ipc_handle_demochar_request(&hdr, buf + sizeof(hdr));
    return 1;
case QUARD_STAR_IPC_SERVICE_LED:
    ipc_handle_led_request(&hdr, buf + sizeof(hdr));
    return 1;
default:
    ipc_send_response(hdr.seq, hdr.service, hdr.opcode,
                      QUARD_STAR_IPC_STATUS_NOSYS, NULL, 0);
    return 1;
}
```

这就是实验三最关键的变化之一：  
`ipc_task` 不再只是“看到字符串然后回显”，而是已经具备一个最小服务核的形态。

### 5.3 legacy 字符串路径

如果收到的 payload 不是合法服务包，则 `ipc_task` 会退回到旧版逻辑：

```c
if (!strncmp((const char *)rx, "ping-", 5))
    len = snprintf(tx, sizeof(tx), "pong-%s", (const char *)rx + 5);
else
    len = snprintf(tx, sizeof(tx), "ack:%s", (const char *)rx);
```

这保证了 `exp2` 的验证方式仍然有效。

## 6. 接入的两个最小服务

### 6.1 demochar 服务

`demochar` 本来就是板子上已有的 MMIO 外设，不是实验三新增硬件。

设备树中原本就有：

- `dts/quard_star.dtsi`

对应节点：

```dts
demochar: demochar@10015000 {
    compatible = "quard,quard-star-demochar";
    reg = <0x0 0x10015000 0x0 0x1000>;
    status = "disabled";
};
```

FreeRTOS 侧通过 MMIO 直接访问：

- `DEMOCHAR_ADDR + 0x0`：数据寄存器
- `DEMOCHAR_ADDR + 0x4`：版本寄存器

关键处理代码：

```c
case QUARD_STAR_IPC_DEMOCHAR_READ:
    value.value = quard_star_readl(DEMOCHAR_ADDR +
                                   QUARD_STAR_DEMOCHAR_DATA_REG);
    return ipc_send_response(..., &value, sizeof(value));

case QUARD_STAR_IPC_DEMOCHAR_WRITE:
    memcpy(&value, payload, sizeof(value));
    quard_star_writel(value.value, DEMOCHAR_ADDR +
                      QUARD_STAR_DEMOCHAR_DATA_REG);
    return ipc_send_response(..., NULL, 0);
```

这意味着：

- `/dev/quard_star_demochar` 仍然是 Linux 直连 MMIO 的原生驱动；
- `/dev/quard_star_demochar_ipc` 则是经过 FreeRTOS 服务代理访问同一个硬件。

### 6.2 LED 服务

实验三中的 `LED` 也不是新加硬件，而是复用了板级已有的 GPIO 背光资源。

板级 DTS 中原本就有：

- `dts/quard_star.dtsi`：GPIO 控制器
- `dts/quard_star_uboot.dts`：`gpio-backlight`

关键定义如下：

```dts
backlight: backlight {
    compatible = "gpio-backlight";
    gpios = <&gpio 0 0>;
    default-on;
    status = "okay";
};
```

因此，实验三把 `gpio0` 当作最小 LED 服务目标。

FreeRTOS 侧核心逻辑：

```c
static void quard_star_led_write(unsigned int led, unsigned int value)
{
    uint32_t bit = 1U << led;
    uint32_t out_en = quard_star_readl(GPIO_ADDR + 0x8);
    uint32_t out_val = quard_star_readl(GPIO_ADDR + 0xc);

    out_en |= bit;
    if (value)
        out_val |= bit;
    else
        out_val &= ~bit;

    quard_star_writel(out_en, GPIO_ADDR + 0x8);
    quard_star_writel(out_val, GPIO_ADDR + 0xc);
}
```

当前只支持：

- `led = 0`
- `value = 0/1`

因此，它本质上是一个布尔开关服务，而不是一个任意数值寄存器镜像。  
写入任意非零值，最终都会被归一化为 `1`。

## 7. OpenSBI Domain 配置

实验三有一个关键细节：  
FreeRTOS 侧不只需要访问 `doorbell` 和 `PLIC`，还需要真正访问 `demochar` 和 `gpio` 的 MMIO。

因此在 `dts/quard_star_sbi.dts` 中新增了 trusted-domain 允许访问的区域：

- `tdemochar`
- `tgpio`

如果不补这一步，`cpu7` 收到请求后虽然会被 doorbell 正常唤醒，但一旦触发 MMIO 访问就会因为 domain 权限问题而失败。

## 8. Linux 侧实现

Linux 侧实现可以分成两个阶段来看。

### 8.1 `exp3-1`：最小同步 IPC 调用

首先，在 `quard_star_ipc` core 中导出了一个最小同步调用入口：

```c
int quard_star_ipc_call(const void *tx, size_t tx_len,
                        void *rx, size_t *rx_len);
```

对应实现位于：

- `linux-6.1.11/drivers/misc/quard_star_ipc.c`
- `linux-6.1.11/include/linux/quard_star_ipc_client.h`

它的语义非常直接：

1. 把请求写入 `linux_to_rtos` ring；
2. 触发 `L2R` doorbell；
3. 阻塞等待 FreeRTOS 回包；
4. 从 `rtos_to_linux` ring 取出一条响应。

这一步把 `exp2` 的原始收发通道变成了一个最小同步 RPC transport。

### 8.2 `exp3-1`：服务代理设备

在 Linux 侧新增两个代理驱动：

- `linux-6.1.11/drivers/misc/quard_star_demochar_ipc.c`
- `linux-6.1.11/drivers/misc/quard_star_led_ipc.c`

并注册出两个新的用户接口：

- `/dev/quard_star_demochar_ipc`
- `/dev/quard_star_led_ipc`

这两个代理驱动的目的不是重复实现硬件逻辑，而是把：

- 用户态读写请求
- 正式服务报文
- `quard_star_ipc_call()`

组合成一个可直接验证的 Linux 侧使用入口。

## 9. `exp3-2`：统一 Linux 请求封装

到 `exp3-1` 为止，服务已经跑通，但 Linux 侧每个 `*_ipc` 驱动仍然要自己做一遍：

- 填消息头；
- 分配 `seq`；
- 拼接请求 buffer；
- 调用 `quard_star_ipc_call()`；
- 解析响应头；
- 校验 `magic/version/type/service/opcode/seq/len/status`。

这会带来两个问题：

1. 所有服务代理驱动都在重复实现同一种协议胶水代码；
2. 后续如果迁移到 `OpenAMP/rpmsg`，这些重复逻辑会成为迁移负担。

因此 `exp3-2` 的目标，是把 Linux 侧服务调用路径进一步收敛到 IPC core 中。

### 9.1 统一请求接口

在 `linux-6.1.11/include/linux/quard_star_ipc_client.h` 中新增：

```c
#define QUARD_STAR_IPC_DEFAULT_TIMEOUT_MS  1000U

struct quard_star_ipc_request {
    unsigned char service;
    unsigned char opcode;
    const void *tx_payload;
    unsigned int tx_len;
    void *rx_payload;
    unsigned int rx_len;
    unsigned int *rx_len_out;
    unsigned int timeout_ms;
};

int quard_star_ipc_request(struct quard_star_ipc_request *req);
```

这个接口把 Linux 服务代理需要关心的内容，压缩成了：

- 发给哪个 service；
- 发哪个 opcode；
- 请求 payload 是什么；
- 预期返回多大 payload；
- 超时是多少。

至于报文头、`seq`、响应校验等公共工作，都由 IPC core 统一完成。

### 9.2 请求级超时

`exp3-2` 还新增了：

```c
int quard_star_ipc_call_timeout(const void *tx, size_t tx_len,
                                void *rx, size_t *rx_len,
                                unsigned int timeout_ms);
```

并让默认调用走：

```c
return quard_star_ipc_call_timeout(tx, tx_len, rx, rx_len,
                                   QUARD_STAR_IPC_DEFAULT_TIMEOUT_MS);
```

这样一来：

- 如果 FreeRTOS 不回包，Linux 不会无限阻塞；
- 请求超时会返回 `-ETIMEDOUT`；
- 服务层已经具备最基本的错误处理边界。

### 9.3 `seq` 与坏包检测

`exp3-2` 中，`seq` 不再由各个代理驱动自己管理，而是统一放进 IPC core：

```c
static atomic_t quard_star_ipc_seq = ATOMIC_INIT(1);
...
tx_hdr.seq = (unsigned int)atomic_inc_return(&quard_star_ipc_seq);
```

同时统一校验响应：

```c
if (rx_hdr.magic != QUARD_STAR_IPC_MSG_MAGIC)
    return -EPROTO;
if (rx_hdr.version != QUARD_STAR_IPC_MSG_VERSION)
    return -EPROTO;
if (rx_hdr.type != QUARD_STAR_IPC_MSG_TYPE_RESP)
    return -EPROTO;
if (rx_hdr.service != tx_hdr.service)
    return -EPROTO;
if (rx_hdr.opcode != tx_hdr.opcode)
    return -EPROTO;
if (rx_hdr.seq != tx_hdr.seq)
    return -EPROTO;
if (rx_len != sizeof(rx_hdr) + rx_hdr.len)
    return -EPROTO;
```

并在内核日志中给出坏包警告：

```c
dev_warn_ratelimited(ipc->miscdev.this_device,
                     "bad IPC response packet: %s\n", reason);
```

这一步的价值是：

- Linux 侧服务代理不再各自维护一份协议校验逻辑；
- 所有协议错误都有统一返回路径；
- 为后续更换 transport 留出了更清晰的抽象层。

### 9.4 串行 request-response 约束

当前 `exp3-2` 的 Linux IPC core 还没有实现真正的多 outstanding request 管理，而是显式采用“串行 request-response”模型：

```c
struct quard_star_ipc {
    ...
    struct mutex call_lock;
    ...
};
```

每次服务调用都先拿 `call_lock`，这样可以确保：

- 一个时刻只存在一个 Linux 发出的同步请求；
- 不会出现多个调用者并发收同一个 response 的问题；
- 当前协议语义简单稳定，适合教学和后续迁移。

这并不意味着以后不能支持并发，而是说明当前阶段还没有把问题复杂化。

## 10. 关键代码位置

实验三的关键代码主要集中在以下文件：

- `ipc_proto/quard_star_ipc.h`
  - 公共协议头、service/opcode/status 定义
- `trusted_domain/main.c`
  - FreeRTOS 服务包解析、dispatcher、demochar/LED 服务处理
- `trusted_domain/driver/quard_star.h`
  - `GPIO_ADDR`、`DEMOCHAR_ADDR` 等 MMIO 地址
- `dts/quard_star_sbi.dts`
  - trusted-domain 对 `gpio` 和 `demochar` 的 MMIO 放行
- `linux-6.1.11/drivers/misc/quard_star_ipc.c`
  - Linux IPC core、同步请求接口、超时、坏包检测
- `linux-6.1.11/include/linux/quard_star_ipc_client.h`
  - Linux 统一 IPC client API
- `linux-6.1.11/drivers/misc/quard_star_demochar_ipc.c`
  - demochar 服务代理
- `linux-6.1.11/drivers/misc/quard_star_led_ipc.c`
  - LED 服务代理

## 11. 构建步骤

实验三分两个阶段，因此构建方式也略有区别。

### 11.1 从 `exp2` 升级到 `exp3-1`

`exp3-1` 修改了：

- Linux kernel
- FreeRTOS trusted-domain
- OpenSBI domain DTS
- firmware

建议按下面顺序重建：

```bash
./build.sh trusted_domain
./build.sh kernel
./build.sh sbi_dtb
./build.sh firmware
./build.sh rootfs bootfs
```

### 11.2 在 `exp3-1` 基础上升级到 `exp3-2`

`exp3-2` 只修改了 Linux kernel 侧接口层，因此只需要重建 kernel：

```bash
./build.sh kernel
./build.sh rootfs bootfs
```

然后重新运行：

```bash
./run.sh customize2
```

## 12. 验证方法

### 12.1 回归 `exp2` 的 legacy ping/pong

先确认 transport 没有回归：

```sh
printf 'ping-0001' > /dev/quard_star_ipc
head -c 9 /dev/quard_star_ipc; echo
```

预期输出：

```text
pong-0001
```

### 12.2 验证 demochar 服务

先通过 IPC 代理写入：

```sh
printf '\x78\x56\x34\x12' > /dev/quard_star_demochar_ipc
```

再分别从 IPC 代理和原始设备读回：

```sh
head -c 4 /dev/quard_star_demochar_ipc | od -An -tx4
head -c 4 /dev/quard_star_demochar | od -An -tx4
```

预期两次都看到：

```text
 12345678
```

### 12.3 验证 LED 服务

关闭：

```sh
printf '\x00\x00\x00\x00' > /dev/quard_star_led_ipc
head -c 4 /dev/quard_star_led_ipc | od -An -tu4
```

打开：

```sh
printf '\x01\x00\x00\x00' > /dev/quard_star_led_ipc
head -c 4 /dev/quard_star_led_ipc | od -An -tu4
```

预期分别看到：

```text
0
1
```

需要注意的是，`LED` 当前是布尔语义：

- 写 `0`，读回 `0`
- 写任意非零值，读回 `1`

例如写入 `0x10` 或 `0x11`，最终读回仍然会是 `1`。

### 12.4 验证 `exp3-2` 的连续请求

可以交替访问 demochar 和 LED，验证统一请求封装后的 Linux 路径是否稳定：

```sh
i=0
while [ $i -lt 20 ]; do
    printf '\x78\x56\x34\x12' > /dev/quard_star_demochar_ipc || break
    head -c 4 /dev/quard_star_demochar_ipc | od -An -tx4 || break
    printf '\x01\x00\x00\x00' > /dev/quard_star_led_ipc || break
    head -c 4 /dev/quard_star_led_ipc | od -An -tu4 || break
    i=$((i + 1))
done
```

如果出现异常，应结合：

```sh
dmesg | tail -n 80
```

重点关注：

- `bad IPC response packet: ...`
- `-ETIMEDOUT`
- `-EPROTO`

## 13. 本实验的意义

实验三的重点不是多接了两个设备，而是把 `exp2` 的“最小消息通道”升级成了一条“最小服务通道”。

完成实验三后，系统已经具备了以下能力：

- transport 层仍然维持共享内存 + doorbell 的最小实现；
- payload 已经具备正式的 `request/response` 语义；
- FreeRTOS 侧已经具备最小 dispatcher；
- Linux 侧已经具备统一的请求封装接口；
- 服务代理驱动可以只关心业务 payload，而不必反复实现协议胶水层。

这正是后续 `exp4` 迁移到 `OpenAMP/rpmsg` 的基础。

届时真正需要替换的主要是 transport 和总线抽象：

- 当前自定义 ring / doorbell / `quard_star_ipc_call`
- 未来的 `vring / rpmsg / endpoint / channel`

而实验三已经沉淀下来的这些内容仍然有价值：

- 服务划分方式
- `service/opcode` 设计
- 请求、响应、错误和超时语义
- Linux 与 FreeRTOS 的职责边界

因此，实验三并不是一个临时过渡版本，而是从“最小 IPC”走向“正式 AMP 服务”的第一步。

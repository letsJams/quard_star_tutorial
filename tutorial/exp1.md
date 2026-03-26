# 基于qemu-riscv从0开始构建异构最小IPC实验exp1. Linux与FreeRTOS共享内存通信

### 实验背景

前面的实验中，`cpu0~6` 运行 Linux，`cpu7` 运行 FreeRTOS，但是两边还没有一个真正可用的最小通信通道。为了验证异构系统中最基础的数据交换能力，本实验实现一个最小版本的 IPC：

- Linux 侧通过字符设备 `/dev/quard_star_ipc` 发送和接收数据。
- FreeRTOS 侧通过一个 `ipc_task` 轮询共享内存，读取 Linux 发来的消息并回复。
- 双方共享一块固定物理地址的内存区域，用两个单生产者单消费者环形队列进行双向收发。

需要特别说明的是，本实验**没有**直接使用跨 domain 的 `SBI IPI` 作为通知机制，而是先采用 **共享内存 + polling** 的方式。原因是当前工程里 Linux 域与 trusted-domain 分属不同 OpenSBI domain，标准 `SEND_IPI` 路径会经过 domain 过滤，而且 Linux 与 FreeRTOS 已各自占用了本域的 `SSIP` 语义，第一版直接复用会把问题复杂化。

因此，本实验的目标很明确：

1. 先打通 Linux 与 FreeRTOS 的最小数据面。
2. 保证代码路径短、便于调试。
3. 为后续升级成 mailbox/doorbell 中断通知打基础。

### 实验内容

本实验完成如下内容：

- 在设备树中预留一段共享内存 `0xBF7F0000 ~ 0xBF7FFFFF`。
- 在 Linux 中新增一个平台 `misc` 驱动 `quard_star_ipc`。
- 在 FreeRTOS trusted_domain 中新增 `ipc.c/ipc.h`，实现共享 ring buffer。
- 将 FreeRTOS 原有的 `task1/task2` 示例替换为 `ipc_task`。
- 使用 `printf 'ping-0001' > /dev/quard_star_ipc` 与 `head -c 9 /dev/quard_star_ipc` 验证双向通信。

### 共享内存结构设计

共享协议头文件放在 `ipc_proto/quard_star_ipc.h`，Linux 与 FreeRTOS 共用同一份定义。

```c
#define QUARD_STAR_IPC_MAGIC        0x51534950U
#define QUARD_STAR_IPC_VERSION      1U
#define QUARD_STAR_IPC_RING_SIZE    2048U
#define QUARD_STAR_IPC_MAX_MSG      256U
#define QUARD_STAR_IPC_SHM_BASE     0xBF7F0000UL
#define QUARD_STAR_IPC_SHM_SIZE     0x00010000UL

struct quard_star_ipc_ring {
    unsigned int prod;
    unsigned int cons;
    unsigned int size;
    unsigned int dropped;
    unsigned char data[QUARD_STAR_IPC_RING_SIZE];
};

struct quard_star_ipc_shared {
    unsigned int magic;
    unsigned int version;
    unsigned int features;
    unsigned int reserved0;
    unsigned int notify_pending_l2r;
    unsigned int notify_pending_r2l;
    unsigned int reserved1;
    unsigned int reserved2;
    struct quard_star_ipc_ring linux_to_rtos;
    struct quard_star_ipc_ring rtos_to_linux;
};
```

说明如下：

- `linux_to_rtos`：Linux 写，FreeRTOS 读。
- `rtos_to_linux`：FreeRTOS 写，Linux 读。
- `prod/cons`：生产者与消费者索引。
- `notify_pending_xxx`：为后续中断版预留，这一版只置位，不实际触发 doorbell。
- 消息格式：`u32长度 + payload`。

### 设备树修改

本实验在 `dts/quard_star_uboot.dts` 中增加两部分内容：

1. `reserved-memory` 下增加共享内存区域。
2. 根节点下增加 `quard_star_ipc` 平台设备节点。

```dts
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    linux,cma@b0000000 {
        compatible = "shared-dma-pool";
        size = <0x0 0xf800000>;
        reg = <0x0 0xb0000000 0x0 0xf800000>;
        alignment = <0x0 0x1000>;
        linux,cma-default;
    };

    ipc_shm: ipc_shm@bf7f0000 {
        reg = <0x0 0xbf7f0000 0x0 0x10000>;
    };
};

quard_star_ipc {
    compatible = "quard,quard-star-ipc";
    memory-region = <&ipc_shm>;
    status = "okay";
};
```

这一步很关键，因为 Linux 侧驱动 probe 依赖这个 `compatible` 和 `memory-region`。

### Linux字符设备驱动

Linux 侧驱动文件为 `linux-6.1.11/drivers/misc/quard_star_ipc.c`，挂接到 `misc` 子系统，因此最终会导出 `/dev/quard_star_ipc`。

驱动核心工作分为三步：

1. 从设备树解析 `memory-region`。
2. 将共享区映射到内核地址空间。
3. 通过 `read/write` 将用户态数据写入 ring 或从 ring 读出。

#### probe流程

```c
memory = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
ret = of_address_to_resource(memory, 0, &res);
base = devm_memremap(&pdev->dev, res.start, resource_size(&res), MEMREMAP_WB);

ipc->shared = base;
quard_star_ipc_init_shared(ipc);

ipc->miscdev.minor = MISC_DYNAMIC_MINOR;
ipc->miscdev.name = "quard_star_ipc";
ipc->miscdev.fops = &quard_star_ipc_fops;
ipc->miscdev.mode = 0666;

ret = misc_register(&ipc->miscdev);
```

这里的 `quard_star_ipc_init_shared()` 会检查共享区中的 `magic/version`，如果无效则重新初始化共享头和 ring 元数据。

#### 写数据路径

```c
ret = quard_star_ipc_push(&ipc->shared->linux_to_rtos,
                          &ipc->shared->notify_pending_l2r,
                          payload, len);
```

它将消息长度和消息体依次写入 `linux_to_rtos`，然后用 `__smp_store_release()` 更新 `prod`，保证数据先写完再让对端看见新索引。

#### 读数据路径

```c
ret = quard_star_ipc_pop(&ipc->shared->rtos_to_linux,
                         &ipc->shared->notify_pending_r2l,
                         payload, cap);
```

如果当前没有完整消息可读，驱动返回 `-EAGAIN`；如果读到损坏的长度字段，则返回 `-EIO`。

### FreeRTOS侧任务实现

FreeRTOS 侧文件为 `trusted_domain/ipc.c` 和 `trusted_domain/main.c`。

#### 初始化共享区

```c
static struct quard_star_ipc_shared *quard_star_ipc_shared_area =
    (struct quard_star_ipc_shared *)QUARD_STAR_IPC_SHM_BASE;

void quard_star_ipc_init(void)
{
    struct quard_star_ipc_shared *shared = quard_star_ipc_shared_area;

    if (shared->magic == QUARD_STAR_IPC_MAGIC &&
        shared->version == QUARD_STAR_IPC_VERSION &&
        shared->linux_to_rtos.size == QUARD_STAR_IPC_RING_SIZE &&
        shared->rtos_to_linux.size == QUARD_STAR_IPC_RING_SIZE)
        return;

    memset(shared, 0, sizeof(*shared));
    quard_star_ipc_init_ring(&shared->linux_to_rtos);
    quard_star_ipc_init_ring(&shared->rtos_to_linux);
    smp_wmb();
    shared->version = QUARD_STAR_IPC_VERSION;
    __smp_store_release(&shared->magic, QUARD_STAR_IPC_MAGIC);
}
```

这部分逻辑与 Linux 侧一致，确保即使先启动哪一侧，看到无效头时都能完成初始化。

#### 最小测试任务

```c
static void ipc_task(void *p_arg)
{
    char rx[QUARD_STAR_IPC_MAX_MSG + 1];
    char tx[QUARD_STAR_IPC_MAX_MSG + 1];
    int len;
    int ret;

    quard_star_ipc_init();
    debug_log("ipc_task ready, shared memory @ 0x%lx\n",
              (unsigned long)QUARD_STAR_IPC_SHM_BASE);

    for (;;) {
        len = quard_star_ipc_recv(rx, QUARD_STAR_IPC_MAX_MSG);
        if (len == QUARD_STAR_IPC_ERR_EMPTY) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        rx[len] = '\0';
        debug_log("ipc rx: %s\n", rx);

        if (!strncmp(rx, "ping-", 5))
            len = snprintf(tx, sizeof(tx), "pong-%s", rx + 5);
        else
            len = snprintf(tx, sizeof(tx), "ack:%s", rx);

        do {
            ret = quard_star_ipc_send(tx, (size_t)len);
            if (ret == QUARD_STAR_IPC_ERR_NOSPC)
                vTaskDelay(pdMS_TO_TICKS(10));
        } while (ret == QUARD_STAR_IPC_ERR_NOSPC);
    }
}
```

这个任务的行为非常简单：

- 有消息就读。
- 如果收到 `ping-xxxx` 就返回 `pong-xxxx`。
- 其他字符串则返回 `ack:原文`。
- 无消息时 `vTaskDelay(10ms)`，避免空转。

### 编译与运行步骤

本实验涉及三条不同的更新链，容易混淆：

1. 修改 Linux 驱动后，需要重新编译内核。
2. 修改设备树后，需要更新 `bootfs` 中的 `quard_star.dtb`。
3. 修改 FreeRTOS trusted_domain 后，需要重新生成 `trusted_fw.bin` 并重新打包 `firmware`。

因此最稳妥的顺序如下：

```bash
./build.sh uboot_dtb
./build.sh kernel
./build.sh trusted_domain
./build.sh firmware
./build.sh rootfs bootfs
```

运行：

```bash
./run.sh customize2
```

建议另开两个终端：

```bash
telnet 127.0.0.1 3441
telnet 127.0.0.1 3443
```

- `3441` 观察 Linux。
- `3443` 观察 FreeRTOS。

### 实验复现现象

正确启动后，Linux 侧可以看到设备节点：

```sh
~ # ls -l /dev/quard_star_ipc
crw-rw----    1 root     root       10, 125 Jan  1  1970 /dev/quard_star_ipc

~ # cat /sys/class/misc/quard_star_ipc/dev
10:125

~ # dmesg | grep -i quard_star_ipc
[    0.943409] quard_star_ipc quard_star_ipc: shared IPC ready at 0x00000000bf7f0000 (/dev/quard_star_ipc)
```

FreeRTOS 串口应出现：

```text
Hello FreeRTOS IPC!
ipc_task ready, shared memory @ 0xbf7f0000
```

发送测试消息：

```sh
printf 'ping-0001' > /dev/quard_star_ipc
head -c 9 /dev/quard_star_ipc; echo
```

Linux 侧预期输出：

```text
pong-0001
```

FreeRTOS 串口预期输出：

```text
ipc rx: ping-0001
```

再测试一个普通字符串：

```sh
printf 'hello' > /dev/quard_star_ipc
head -c 9 /dev/quard_star_ipc; echo
```

预期：

```text
ack:hello
```

### 调试思路与踩坑记录

本实验真正花时间的不是代码本身，而是更新链路和运行现象的判断。这里记录最关键的几个排障点。

#### 1. `/dev/quard_star_ipc` 最开始显示成普通文件

错误现象：

```sh
-rw-r--r-- 1 root root 9 ... /dev/quard_star_ipc
```

这不是驱动节点，而是因为设备节点并不存在时执行了：

```sh
printf 'ping-0001' > /dev/quard_star_ipc
```

shell 直接在 `/dev` 下创建了一个普通文件。这个现象本身就说明 **驱动没有 probe**。

正确判断标准应该是：

```sh
crw-rw---- ... /dev/quard_star_ipc
```

也就是文件类型必须是 `c`，而不是 `-`。

#### 2. Linux驱动已经编进去，但 `quard_star_ipc` 没有出现在 `/sys/class/misc`

这一步说明：

- misc 子系统正常。
- 驱动框架正常。
- 只是新平台设备没有被创建设备树节点匹配到。

此时要重点看：

```sh
find /sys/firmware/devicetree/base -name '*ipc*'
```

如果完全没有结果，说明运行时 dtb 仍是旧的。

#### 3. `output/uboot/quard_star_uboot.dtb` 已更新，但运行时仍无新节点

这是本实验最容易误判的一点。

工程里真正传给 Linux 的 dtb 不只取决于 `output/uboot/quard_star_uboot.dtb` 是否更新，还取决于是否把它重新拷贝进了 `bootfs`：

```bash
./build.sh rootfs bootfs
```

如果只改了 `dts`，但没有刷新 `bootfs`，运行时仍会加载旧的 `/quard_star.dtb`。

验证方法：

```bash
strings output/uboot/quard_star_uboot.dtb | grep quard_star_ipc
strings output/rootfs/bootfs/quard_star.dtb | grep quard_star_ipc
```

如果前者有、后者没有，说明问题就在 `bootfs` 没更新。

#### 4. Linux侧字符设备正常，但通信仍返回 `Input/output error`

这是第二个关键排障点。现象如下：

```sh
printf 'ping-0001' > /dev/quard_star_ipc
head -c 9 /dev/quard_star_ipc; echo
head: /dev/quard_star_ipc: Input/output error
```

此时 Linux 驱动其实已经工作了，但 FreeRTOS 仍在跑旧的 `task1/task2` 示例程序。说明 trusted_domain 没更新到新的 `ipc_task` 版本。

因此需要重新执行：

```bash
./build.sh trusted_domain
./build.sh firmware
```

这里必须注意：

- `trusted_domain` 只生成 `trusted_fw.bin`
- `firmware` 才会把 `trusted_fw.bin` 重新打包进 `fw.bin/sd.img`

如果漏掉 `firmware`，QEMU 启动时 `cpu7` 仍然会加载旧固件。

### 实验结论

本实验完成了一个最小可验证的异构 IPC 通道：

- Linux 侧通过 `/dev/quard_star_ipc` 与对端通信。
- FreeRTOS 侧通过 `ipc_task` 从共享内存读取消息并回包。
- 共享内存地址固定，协议简单，易于调试。
- 第一版不依赖跨 domain IPI，因此更适合作为基础实验。

这个实验的价值不在于功能复杂，而在于把下面这条链路完整打通：

1. 设备树预留共享内存。
2. Linux 平台驱动 + 字符设备导出。
3. FreeRTOS 共享 ring buffer 处理。
4. 固件、bootfs、rootfs 三条更新链的区别。
5. 通过串口日志和字符设备现象判断问题所在。

后续如果要继续扩展，可以考虑两条路线：

- 增加 doorbell/mailbox 机制，将 polling 改为中断通知。
- 在 Linux 侧增加更完整的阻塞读、poll/epoll、消息统计和错误恢复逻辑。

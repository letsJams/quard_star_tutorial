# 实验二：基于 Doorbell 的双向异构核中断通信

## 1. 实验背景

在实验一中，已经实现了 Linux 与 FreeRTOS 之间基于共享内存的最小 IPC 通信，但当时的数据收发仍然依赖轮询：

- Linux 把消息写入共享内存；
- FreeRTOS 任务周期性轮询共享区，发现新数据后进行处理；
- 回包后 Linux 再主动读取。

这种方式虽然简单，但存在两个明显问题：

1. 轮询会浪费 CPU 时间；
2. 收发延迟取决于轮询周期，不够及时。

因此，本实验在实验一的共享内存数据面基础上，引入一个最小的 `doorbell` 外设，构建一条真正的“事件驱动”异构通信链：

- 共享内存负责传输数据；
- doorbell 负责发中断通知；
- Linux 和 FreeRTOS 双方都通过中断来唤醒对端。

这也是实际 AMP/异构系统中非常常见的一种设计方式：

- 数据面：shared memory
- 通知面：mailbox / doorbell / IPI

本实验选择 `doorbell`，是因为它比完整 mailbox 控制器更轻量，适合教学和最小实现验证。

## 2. 实验目标

本实验的目标是实现一条最小但完整的双向异构中断通信链：

1. Linux 向共享内存写入请求消息；
2. Linux 通过 `L2R` doorbell 中断通知 FreeRTOS；
3. FreeRTOS 被外部中断唤醒，读取共享内存中的请求；
4. FreeRTOS 处理消息并写回共享内存；
5. FreeRTOS 通过 `R2L` doorbell 中断通知 Linux；
6. Linux 的阻塞 `read()` 被唤醒，读出返回数据。

最终形成：

- Linux -> FreeRTOS：中断驱动
- FreeRTOS -> Linux：中断驱动
- 双向数据交换仍基于共享内存 ring buffer

## 3. 整体设计

### 3.1 分层思路

本实验没有推翻实验一的数据结构，而是继续沿用共享内存 ring，只增加中断通知层。

架构分为两层：

1. 数据面：共享内存
2. 通知面：doorbell

具体流程如下：

1. Linux `write()` 把 `ping-xxxx` 写入 `linux_to_rtos` ring；
2. Linux 写 doorbell `L2R_SET`；
3. QEMU 中的 doorbell 外设拉起 PLIC 中断源；
4. FreeRTOS 收到外部中断后唤醒 `ipc_task`；
5. `ipc_task` 从共享内存读取消息并生成 `pong-xxxx`；
6. FreeRTOS 把响应写入 `rtos_to_linux` ring；
7. FreeRTOS 写 doorbell `R2L_SET`；
8. Linux 中断处理函数运行，唤醒阻塞中的 `read()`；
9. Linux 读出返回消息。

### 3.2 Doorbell 与 Mailbox 的关系

本实验实现的是 `doorbell`，不是完整 mailbox。

区别如下：

- `doorbell` 只负责通知“有事件发生了”；
- `mailbox` 通常是更完整的消息硬件抽象，可能包含 channel、FIFO、状态机、ack 等机制。

本实验已经有共享内存承担数据传输，因此只需要一个最小的 doorbell 负责中断通知即可，不必额外设计一套 mailbox 数据寄存器。

## 4. QEMU 侧设计与实现

### 4.1 Doorbell 寄存器设计

QEMU 中新增了一个最小 MMIO 外设 `quard_star_doorbell`。其寄存器设计如下：

```c
0x00 L2R_SET      // Linux -> RTOS 置位
0x04 L2R_CLR      // RTOS 清除
0x08 R2L_SET      // RTOS -> Linux 置位
0x0C R2L_CLR      // Linux 清除
0x10 STATUS       // bit0: L2R pending, bit1: R2L pending
```

关键点：

- `L2R` 用于 Linux 通知 FreeRTOS；
- `R2L` 用于 FreeRTOS 通知 Linux；
- pending 位采用电平触发语义，只有写 `CLR` 后中断才真正撤销。

### 4.2 关键代码

QEMU 设备的核心状态定义在：

- [qemu-8.0.0/include/hw/misc/quard_star_doorbell.h](/home/uwlab/mysoft/quard_star/quard_star_tutorial/qemu-8.0.0/include/hw/misc/quard_star_doorbell.h)

核心逻辑在：

- [qemu-8.0.0/hw/misc/quard_star_doorbell.c](/home/uwlab/mysoft/quard_star/quard_star_tutorial/qemu-8.0.0/hw/misc/quard_star_doorbell.c)

其中最关键的是根据 `pending` 位更新两路 IRQ：

```c
static void quard_star_doorbell_update_irq(QuardStarDoorbellState *s)
{
    qemu_set_irq(s->irq_l2r,
                 !!(s->status & QUARD_STAR_DOORBELL_L2R_PENDING));
    qemu_set_irq(s->irq_r2l,
                 !!(s->status & QUARD_STAR_DOORBELL_R2L_PENDING));
}
```

对应的寄存器写处理逻辑：

```c
switch (addr) {
case QUARD_STAR_DOORBELL_L2R_SET:
    s->status |= QUARD_STAR_DOORBELL_L2R_PENDING;
    break;
case QUARD_STAR_DOORBELL_L2R_CLR:
    s->status &= ~QUARD_STAR_DOORBELL_L2R_PENDING;
    break;
case QUARD_STAR_DOORBELL_R2L_SET:
    s->status |= QUARD_STAR_DOORBELL_R2L_PENDING;
    break;
case QUARD_STAR_DOORBELL_R2L_CLR:
    s->status &= ~QUARD_STAR_DOORBELL_R2L_PENDING;
    break;
}
quard_star_doorbell_update_irq(s);
```

### 4.3 板级接入

`doorbell` 被挂入 Quard Star 板级模型，并分配两个 PLIC source：

- `60`：Linux -> FreeRTOS
- `61`：FreeRTOS -> Linux

相关代码：

- [qemu-8.0.0/include/hw/riscv/quard_star.h](/home/uwlab/mysoft/quard_star/quard_star_tutorial/qemu-8.0.0/include/hw/riscv/quard_star.h)
- [qemu-8.0.0/hw/riscv/quard_star.c](/home/uwlab/mysoft/quard_star/quard_star_tutorial/qemu-8.0.0/hw/riscv/quard_star.c)

## 5. 设备树与 OpenSBI Domain 配置

### 5.1 Linux 使用的 DTS

Linux 使用的设备树中，给 doorbell 节点分配了双中断：

- [dts/quard_star.dtsi](/home/uwlab/mysoft/quard_star/quard_star_tutorial/dts/quard_star.dtsi)

关键定义：

```dts
doorbell: doorbell@10016000 {
    compatible = "quard,quard-star-doorbell";
    reg = <0x0 0x10016000 0x0 0x1000>;
    interrupts = <60>, <61>;
    interrupt-names = "l2r", "r2l";
    interrupt-parent = <&plic>;
    status = "disabled";
};
```

在 Linux 侧 IPC 平台节点中，还增加了对 doorbell 的引用：

- [dts/quard_star_uboot.dts](/home/uwlab/mysoft/quard_star/quard_star_tutorial/dts/quard_star_uboot.dts)

```dts
quard_star_ipc {
    compatible = "quard,quard-star-ipc";
    memory-region = <&ipc_shm>;
    doorbell = <&doorbell>;
    interrupt-parent = <&plic>;
    interrupts = <61>;
};
```

这里 Linux 只关注 `61`，因为这是 FreeRTOS 回包时通知 Linux 的反向 doorbell。

### 5.2 OpenSBI Domain 的 MMIO 放行

这一步是调试中最关键的坑之一。

FreeRTOS 运行在 `cpu7` 的 trusted-domain 中，而一开始 OpenSBI 并没有给 trusted-domain 放行 `doorbell` 和 `PLIC` 的 MMIO 区域，导致 `cpu7` 一访问 doorbell 寄存器就异常。

解决方法是在：

- [dts/quard_star_sbi.dts](/home/uwlab/mysoft/quard_star/quard_star_tutorial/dts/quard_star_sbi.dts)

里显式加入：

- `tplic`
- `tdoorbell`

并把它们添加到 trusted-domain 的 `regions` 中。这样 `cpu7` 才能访问：

- `0x0c000000` PLIC
- `0x10016000` doorbell

## 6. FreeRTOS 侧实现

### 6.1 中断初始化

FreeRTOS 侧新增：

- [trusted_domain/doorbell.h](/home/uwlab/mysoft/quard_star/quard_star_tutorial/trusted_domain/doorbell.h)
- [trusted_domain/doorbell.c](/home/uwlab/mysoft/quard_star/quard_star_tutorial/trusted_domain/doorbell.c)

初始化内容包括：

1. 清 `L2R` doorbell 源；
2. 配置 PLIC source 60 priority；
3. 设置 threshold；
4. 使能 `cpu7` 对应 S-mode context 的 source 60；
5. 打开 `SEIP`。

关键点在于 `cpu7` 使用的是 S-mode context，而不是 M-mode context。

### 6.2 中断处理顺序

由于 doorbell 在 QEMU 中采用的是“电平触发语义”，所以中断清理顺序必须严格正确。

错误顺序是：

1. 先 `PLIC complete`
2. 再清 doorbell 源

这样如果源电平还在，PLIC 会立刻再次触发。

因此正确顺序必须是：

1. 先清 doorbell 源；
2. 再执行 `PLIC complete`。

核心逻辑：

```c
irq = plic_claim();
if (irq == QUARD_STAR_DOORBELL_L2R_IRQ) {
    quard_star_doorbell_clear_l2r();
    plic_complete(irq);
    vTaskNotifyGiveFromISR(g_ipc_task, &xHigherPriorityTaskWoken);
}
```

### 6.3 FreeRTOS 任务模型

`ipc_task` 不再轮询共享内存，而是阻塞等待 doorbell 唤醒。

文件：

- [trusted_domain/main.c](/home/uwlab/mysoft/quard_star/quard_star_tutorial/trusted_domain/main.c)

关键逻辑：

```c
for (;;) {
    ret = quard_star_ipc_recv(buf, sizeof(buf));
    if (ret == -EAGAIN) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        debug("ipc_task woke by doorbell\n");
        continue;
    }

    debug("ipc rx: %s\n", buf);
    quard_star_ipc_send(reply, len);
    quard_star_doorbell_ring_r2l();
}
```

这段代码体现了本实验最核心的思想：

- 中断只负责唤醒任务；
- 任务在正常上下文中处理共享内存数据；
- 回包后再主动触发 `R2L` doorbell。

## 7. Linux 侧实现

### 7.1 Linux -> FreeRTOS：写共享内存并敲门

Linux 驱动文件：

- [linux-6.1.11/drivers/misc/quard_star_ipc.c](/home/uwlab/mysoft/quard_star/quard_star_tutorial/linux-6.1.11/drivers/misc/quard_star_ipc.c)

在 `write()` 路径中，成功写入共享 ring 后，会立即触发 `L2R_SET`：

```c
ret = quard_star_ipc_push(&ipc->shared->linux_to_rtos, kbuf, len);
if (!ret)
    quard_star_ipc_ring_doorbell_l2r(ipc);
```

这就把原来的“写共享内存 + 对端轮询发现”改成了“写共享内存 + 立即中断通知”。

### 7.2 FreeRTOS -> Linux：中断唤醒阻塞读

第四步的新增重点在于 Linux 收 `R2L` doorbell。

驱动里新增了：

1. `doorbell` MMIO 映射；
2. `request_irq()`；
3. `wait_queue_head_t readq`；
4. 阻塞式 `read()`；
5. `irq handler` 中清 doorbell 并 `wake_up_interruptible()`。

IRQ handler 关键逻辑：

```c
static irqreturn_t quard_star_ipc_irq(int irq, void *data)
{
    struct quard_star_ipc *ipc = data;

    quard_star_ipc_clear_doorbell_r2l(ipc);
    wake_up_interruptible(&ipc->readq);
    return IRQ_HANDLED;
}
```

阻塞读逻辑：

```c
for (;;) {
    mutex_lock(&ipc->lock);
    ret = quard_star_ipc_pop(&ipc->shared->rtos_to_linux, kbuf, &len);
    mutex_unlock(&ipc->lock);

    if (!ret)
        break;

    if (file->f_flags & O_NONBLOCK)
        return -EAGAIN;

    ret = wait_event_interruptible(ipc->readq,
                                   quard_star_ipc_r2l_ready(ipc));
    if (ret)
        return ret;
}
```

这样 Linux 侧的 `read()` 就不再是主动轮询，而是被 FreeRTOS 回包的中断唤醒。

## 8. 实验步骤与复现方法

### 8.1 编译顺序

由于本实验同时修改了 QEMU、Linux DTB、Linux kernel、FreeRTOS 和 OpenSBI domain 配置，因此必须按正确顺序刷新运行产物：

```bash
./build.sh qemu
./build.sh uboot_dtb
./build.sh kernel
./build.sh rootfs bootfs
./build.sh trusted_domain
./build.sh sbi_dtb
./build.sh firmware
```

### 8.2 运行方式

启动前建议先退出旧 QEMU：

```bash
pkill -f qemu-system-riscv64
```

然后运行：

```bash
./run.sh customize2
```

建议将 `3443` 串口配置改为 `server,wait`，这样可以先连 FreeRTOS 串口，避免丢失早期日志。

### 8.3 验证单次通信

在 Linux shell 中执行：

```sh
printf 'ping-0001' > /dev/quard_star_ipc
```

在 FreeRTOS 串口中应看到：

```text
ipc_task woke by doorbell
ipc rx: ping-0001
```

然后验证 Linux 阻塞读是否被反向中断唤醒：

```sh
head -c 9 /dev/quard_star_ipc > /tmp/ipc.out &
printf 'ping-0004' > /dev/quard_star_ipc
wait
cat /tmp/ipc.out; echo
```

预期输出：

```text
pong-0004
```

同时观察 Linux 中断计数：

```sh
cat /proc/interrupts | grep -i -E 'ipc|doorbell|plic'
```

应看到 `quard_star_ipc` 对应的 `IRQ 61` 计数增加，例如：

```text
37:          1          0          0          0          0          0          0  SiFive PLIC  61 Edge      quard_star_ipc
```

这说明 Linux 确实收到了 FreeRTOS 回包触发的反向中断。

## 9. 压力测试

为验证双向中断通信是否稳定，编写了一个简单的压力测试脚本：

```sh
#!/bin/sh
set -eu

DEV="${1:-/dev/quard_star_ipc}"
COUNT="${2:-100}"
REPLY_LEN=9
TMP_FILE="/tmp/ipc_reply.$$"

cleanup() {
    rm -f "$TMP_FILE"
}
trap cleanup EXIT INT TERM

irq_before="$(grep quard_star_ipc /proc/interrupts | awk '{print $2}')"
irq_before="${irq_before:-0}"

i=1
while [ "$i" -le "$COUNT" ]; do
    msg="$(printf 'ping-%04d' "$i")"
    expect="$(printf 'pong-%04d' "$i")"

    head -c "$REPLY_LEN" "$DEV" > "$TMP_FILE" &
    reader_pid=$!

    printf '%s' "$msg" > "$DEV"
    wait "$reader_pid"

    reply="$(cat "$TMP_FILE")"
    [ "$reply" = "$expect" ] || exit 1

    i=$((i + 1))
done

irq_after="$(grep quard_star_ipc /proc/interrupts | awk '{print $2}')"
echo "delta=$((irq_after - irq_before))"
```

### 9.1 实测结果

在 QEMU 中实际运行结果如下：

```text
/tmp # ./ipc_stress.sh /dev/quard_star_ipc 50
stress start: dev=/dev/quard_star_ipc count=50 irq_before=0
iter=10 reply=pong-0010 irq=10
iter=20 reply=pong-0020 irq=20
iter=30 reply=pong-0030 irq=30
iter=40 reply=pong-0040 irq=40
iter=50 reply=pong-0050 irq=50
stress pass: count=50 irq_before=0 irq_after=50
  delta=50
```

```text
/tmp # ./ipc_stress.sh /dev/quard_star_ipc 100
stress start: dev=/dev/quard_star_ipc count=100 irq_before=50
iter=10 reply=pong-0010 irq=60
iter=20 reply=pong-0020 irq=70
iter=30 reply=pong-0030 irq=80
iter=40 reply=pong-0040 irq=90
iter=50 reply=pong-0050 irq=100
iter=60 reply=pong-0060 irq=110
iter=70 reply=pong-0070 irq=120
iter=80 reply=pong-0080 irq=130
iter=90 reply=pong-0090 irq=140
iter=100 reply=pong-0100 irq=150
stress pass: count=100 irq_before=50 irq_after=150
  delta=100
```

这说明：

1. 每一次 ping-pong 都成功完成；
2. 每一次回包都对应一个 Linux 侧 `IRQ 61` 中断；
3. 没有出现丢包、串包、死锁。

注意第二次测试中 `irq_before` 不会从 `0` 开始，而是从上一次测试结束后的累计值开始。这是因为 `/proc/interrupts` 统计的是内核运行以来的总中断次数，而不是单次测试的局部值。

## 10. 调试过程中遇到的关键问题

### 10.1 QEMU 构建目录与运行目录不一致

一开始虽然已经重新编译了 `qemu-8.0.0/build`，但 `run.sh` 实际运行的是：

- `output/qemu/bin/qemu-system-riscv64`

因此即使源码改了，如果没有执行：

```bash
./build.sh qemu
```

运行时仍然会使用旧的 QEMU 二进制，导致 doorbell 设备看似“没有生效”。

这是本实验中第一个关键坑。

### 10.2 OpenSBI domain 没有放行 MMIO

FreeRTOS 所在 `cpu7` 属于 trusted-domain，一开始 trusted-domain 没有被授权访问：

- `doorbell` MMIO
- `PLIC` MMIO

因此在 `doorbell_init()` 中第一次访问 doorbell 寄存器时就会卡住。现象上表现为：

```text
doorbell_init: before clear source
```

之后再无输出。

最终通过给 trusted-domain 增加：

- `tplic`
- `tdoorbell`

两个 region 才解决问题。

### 10.3 早期日志丢失

FreeRTOS 串口默认是 `server,nowait`，如果连接 `3443` 太晚，会错过启动早期日志，容易误判为“代码没有运行到这里”。

因此调试阶段将 FreeRTOS 串口改成 `server,wait`，先连串口再启动，有助于准确观察启动过程。

### 10.4 中断清理时序错误的风险

本实验中特别注意了 doorbell 的中断清理顺序。

如果顺序写错为：

1. 先 `PLIC complete`
2. 再清 `doorbell`

那么由于源仍为电平有效，PLIC 会立即再次触发。

因此必须严格保证：

1. 先清 doorbell 源
2. 再执行 PLIC complete

这是异构中断通信里非常容易踩的一个坑。

## 11. 实验结论

本实验在实验一共享内存 IPC 的基础上，进一步实现了基于 `doorbell` 的双向中断通知，形成了一条完整的异构事件驱动通信链：

- Linux -> FreeRTOS：共享内存 + `L2R` doorbell
- FreeRTOS -> Linux：共享内存 + `R2L` doorbell

实验结果表明：

1. 双向中断通知链路工作正常；
2. FreeRTOS 能被 Linux 写操作立即唤醒；
3. Linux 阻塞读能被 FreeRTOS 回包中断唤醒；
4. 在 50 次、100 次 ping-pong 压力测试中均未出现丢包、死锁、串包；
5. `/proc/interrupts` 中 `quard_star_ipc` 对应 IRQ 61 的累计计数与测试轮数一致，证明反向 doorbell 中断链可靠。

因此，可以认为当前工程已经完成了一个最小但完整的“共享内存 + 双向 doorbell 中断”异构通信原型。

## 12. 后续扩展方向

在此基础上，可以继续向以下方向扩展：

1. 增加 `irq_pending`/批处理机制，减少高频小包造成的中断次数；
2. 将当前私有 doorbell 机制进一步抽象成 Linux mailbox controller；
3. 增加更正式的延迟/吞吐测试；
4. 增加异常恢复、超时和错误统计；
5. 在真实硬件环境中补充 cache maintenance 与一致性处理。

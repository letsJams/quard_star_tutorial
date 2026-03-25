// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Quard Star demo character device driver
 * dts/quard_star_uboot.dts dts/quard_star.dtsi \
 * linux6.1.11/arch/riscv/configs/quard_star_defconfig \
 * linux6.1.11/arch/drivers/misc/Kconfig \
 * linux6.1.11/arch/drivers/misc/Makefile \
 */

#include <linux/fs.h>              // struct file, file_operations
#include <linux/io.h>              // readl/writel, ioremap 相关接口
#include <linux/kernel.h>          // 内核基础宏/接口
#include <linux/miscdevice.h>      // miscdevice 框架
#include <linux/module.h>          // module_platform_driver/MODULE_* 宏
#include <linux/of.h>              // 设备树匹配
#include <linux/platform_device.h> // platform_driver/platform_device
#include <linux/slab.h>            // 内存分配
#include <linux/uaccess.h>         // copy_to_user/copy_from_user

#define QUARD_STAR_DEMOCHAR_DATA_REG    0x0         //数据寄存器
#define QUARD_STAR_DEMOCHAR_VERSION_REG 0x4         //版本寄存器
#define QUARD_STAR_DEMOCHAR_VERSION     0x00010000  //驱动期望版本寄存器返回 0x00010000

struct quard_star_demochar {        //设备私有结构体
    void __iomem *base;             // MMIO 基址，指向设备寄存器空间，映射后的虚拟基地址
    struct miscdevice miscdev;      // misc 字符设备对象
};

static ssize_t quard_star_demochar_read(struct file *file, char __user *buf,
                                        size_t len, loff_t *ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_demochar *demochar;
    u32 value;

    if (*ppos != 0) //position ptr表示文件读写偏移量
        return 0;

    if (len < sizeof(value))
        return -EINVAL;

    demochar = container_of(miscdev, struct quard_star_demochar, miscdev);//由已知成员反推父结构体基地址
    value = readl(demochar->base + QUARD_STAR_DEMOCHAR_DATA_REG);

    if (copy_to_user(buf, &value, sizeof(value)))   //copy_to_user()内核空间到用户空间的拷贝
        return -EFAULT;

    *ppos += sizeof(value);
    return sizeof(value);
}

static ssize_t quard_star_demochar_write(struct file *file,
                                         const char __user *buf,
                                         size_t len, loff_t *ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct quard_star_demochar *demochar;
    u32 value;

    if (len < sizeof(value))
        return -EINVAL;

    if (copy_from_user(&value, buf, sizeof(value))) //copy_from_user()从用户空间向内核空间拷贝
        return -EFAULT;

    demochar = container_of(miscdev, struct quard_star_demochar, miscdev);
    writel(value, demochar->base + QUARD_STAR_DEMOCHAR_DATA_REG);

    return sizeof(value);
}

static const struct file_operations quard_star_demochar_fops = {    //VFS的核心结构体，定义文件操作表
    .owner = THIS_MODULE,
    .read = quard_star_demochar_read,
    .write = quard_star_demochar_write,
    .llseek = no_llseek,    //只能顺序读写
};

//平台设备platform_device *pdev是linux中内置设备的抽象，通常通过device tree在启动时创建
static int quard_star_demochar_probe(struct platform_device *pdev)  //设备探测函数probe()
{
    struct quard_star_demochar *demochar;
    struct resource *res;
    u32 version;
    int ret;

    //devm_前缀的函数会将资源和struct device生命周期绑定，设备移除会自动释放内存
    demochar = devm_kzalloc(&pdev->dev, sizeof(*demochar), GFP_KERNEL);     //devm_kzalloc()托管式内存分配
    if (!demochar)
        return -ENOMEM;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);   //从 platform 设备拿第 0 个 MMIO 资源
    demochar->base = devm_ioremap_resource(&pdev->dev, res);//把这段物理地址映射成内核可访问的 __iomem 地址
    if (IS_ERR(demochar->base))
        return PTR_ERR(demochar->base);

    version = readl(demochar->base + QUARD_STAR_DEMOCHAR_VERSION_REG);//硬件版本检查
    if (version != QUARD_STAR_DEMOCHAR_VERSION)
        dev_warn(&pdev->dev, "unexpected version: 0x%08x\n", version);

    demochar->miscdev.minor = MISC_DYNAMIC_MINOR;       //自动分配次设备号
    demochar->miscdev.name = "quard_star_demochar";     //设备名，将创建设备“/dev/quard_star_demochar”
    demochar->miscdev.fops = &quard_star_demochar_fops; //绑定操作函数
    demochar->miscdev.parent = &pdev->dev;              //父设备设为当前 platform 设备

    ret = misc_register(&demochar->miscdev);            //向内核注册混杂函数
    if (ret)
        return ret;

    platform_set_drvdata(pdev, demochar);
    dev_info(&pdev->dev, "registered /dev/%s\n", demochar->miscdev.name);

    return 0;
}

//设备移除函数
static int quard_star_demochar_remove(struct platform_device *pdev)
{
    struct quard_star_demochar *demochar = platform_get_drvdata(pdev);

    misc_deregister(&demochar->miscdev);    //注销misc设备，由于使用了devm_kzalloc,无需显式iounmap
    return 0;
}

//设备树匹配表，用于device tree匹配
static const struct of_device_id quard_star_demochar_of_match[] = {
    { .compatible = "quard,quard-star-demochar" },
    { }
};
MODULE_DEVICE_TABLE(of, quard_star_demochar_of_match);

//platform_driver 注册

static struct platform_driver quard_star_demochar_driver = {//平台驱动结构体与模块宏，platform_driver平台设备驱动结构体
    .probe = quard_star_demochar_probe,
    .remove = quard_star_demochar_remove,
    .driver = {
        .name = "quard_star_demochar",
        .of_match_table = quard_star_demochar_of_match,
    },
};
module_platform_driver(quard_star_demochar_driver);

MODULE_AUTHOR("OpenAI");
MODULE_DESCRIPTION("Quard Star demo char driver");
MODULE_LICENSE("GPL");

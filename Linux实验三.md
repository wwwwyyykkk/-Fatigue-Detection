# 实验三：GPIO 测试实验操作步骤

## 实验环境

- 开发板：KICKPI RK3568（K1）
- 虚拟机：Ubuntu 24.04（VMware）
- SDK：rk356x-linux-20251212
- 运行灯引脚：GPIO0_D5（gpio29）

---

## 一、准备工作

### 1.1 获取并解压SDK

```bash
# 将SDK压缩包复制到虚拟机本地磁盘（不能在共享目录下编译）
cp /mnt/hgfs/20251212/rk356x-linux-20251212.tar.gz /mnt/sdkdisk/

# 解压
cd /mnt/sdkdisk
tar -zxvf rk356x-linux-20251212.tar.gz

# 进入SDK目录，还原源码
cd rk356x-linux
git reset --hard
```

### 1.2 安装编译依赖

```bash
sudo apt-get install python3 python-is-python3 lz4 libssl-dev \
libgmp-dev libmpc-dev libmpfr-dev bc flex bison \
device-tree-compiler libelf-dev pkg-config -y
```

### 1.3 lunch配置

```bash
./build.sh lunch
# 选择 1（rk3566_rk3568）
# 选择 6（rockchip_rk3568_kickpi_k1_debian_defconfig）
```

---

## 二、修改设备树（释放运行灯）

运行灯默认被内核 heartbeat 驱动占用，需要注释掉。

```bash
vi kernel/arch/arm64/boot/dts/rockchip/rk3568-kickpi-evb.dtsi
```

用 `/heartbeat` 搜索，找到 work_led 节点，注释掉：

```dts
leds: leds {
    compatible = "gpio-leds";

    /* work_led: work-led {
        gpios = <&gpio0 RK_PD5 GPIO_ACTIVE_HIGH>;
        linux,default-trigger = "heartbeat";
    } ; */
};
```

保存退出（`:wq`）。

---

## 三、编译内核

```bash
./build.sh kernel
```

编译成功后镜像路径：
```
/mnt/sdkdisk/rk356x-linux/kernel/boot.img
```

---

## 四、编写驱动代码

### 4.1 关键寄存器说明

GPIO0_D5 对应寄存器（RK3568）：

| 寄存器 | 地址 | 说明 |
|--------|------|------|
| 复用寄存器 GPIO0D_IOMUX_H | `0xFDC2001C` | bit8-10清零设为GPIO，写使能bit24-26 |
| 方向寄存器 GPIO_SWPORT_DDR_H | `0xFDD6000C` | bit13=1设为输出，写使能bit29 |
| 数据寄存器 GPIO_SWPORT_DR_H | `0xFDD60004` | bit13=1高电平，写使能bit29 |

> **注意**：D组引脚在_H寄存器里从bit8开始排列，D5 = 8+5 = **bit13**

### 4.2 创建驱动文件

```bash
mkdir /mnt/sdkdisk/gpio_led
vi /mnt/sdkdisk/gpio_led/gpio_led_drv.c
```

驱动代码内容：

```c
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>

#define DEVICE_NAME     "gpio_led"
#define MUX_REG         0xFDC2001C   /* 复用寄存器 GPIO0D_IOMUX_H */
#define DIR_REG         0xFDD6000C   /* 方向寄存器 */
#define DATA_REG        0xFDD60004   /* 数据寄存器 */

#define LED_MAGIC   'L'
#define LED_ON      _IO(LED_MAGIC, 0)
#define LED_OFF     _IO(LED_MAGIC, 1)

static dev_t devno;
static struct cdev gpio_cdev;
static struct class *gpio_class;
static void __iomem *mux_base;
static void __iomem *dir_base;
static void __iomem *data_base;

static int gpio_led_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int gpio_led_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t gpio_led_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos)
{
    char val;
    u32 data;

    if (copy_from_user(&val, buf, 1))
        return -EFAULT;

    data = readl(data_base);
    if (val == '1')
        data = (data | (1 << 13)) | (1u << 29);
    else
        data = (data & ~(1 << 13)) | (1u << 29);

    writel(data, data_base);
    return count;
}

static long gpio_led_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg)
{
    u32 data;

    switch (cmd) {
    case LED_ON:
        data = readl(data_base);
        data = (data | (1 << 13)) | (1u << 29);
        writel(data, data_base);
        printk("LED ON\n");
        break;
    case LED_OFF:
        data = readl(data_base);
        data = (data & ~(1 << 13)) | (1u << 29);
        writel(data, data_base);
        printk("LED OFF\n");
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static struct file_operations gpio_led_fops = {
    .owner          = THIS_MODULE,
    .open           = gpio_led_open,
    .release        = gpio_led_release,
    .write          = gpio_led_write,
    .unlocked_ioctl = gpio_led_ioctl,
};

static int __init gpio_led_init(void)
{
    u32 val;

    alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    cdev_init(&gpio_cdev, &gpio_led_fops);
    gpio_cdev.owner = THIS_MODULE;
    cdev_add(&gpio_cdev, devno, 1);

    gpio_class = class_create(THIS_MODULE, DEVICE_NAME);
    device_create(gpio_class, NULL, devno, NULL, DEVICE_NAME);

    mux_base  = ioremap(MUX_REG,  4);
    dir_base  = ioremap(DIR_REG,  4);
    data_base = ioremap(DATA_REG, 4);

    /* 复用寄存器：D5设为GPIO功能，bit8-10清零，写使能bit24-26 */
    val = readl(mux_base);
    val &= ~(0x7 << 8);
    val |=  (0x7u << 24);
    writel(val, mux_base);

    /* 方向寄存器：D5设为输出，bit13=1，写使能bit29 */
    val = readl(dir_base);
    val |= (1u << 29) | (1 << 13);
    writel(val, dir_base);

    printk("gpio_led driver loaded\n");
    return 0;
}

static void __exit gpio_led_exit(void)
{
    iounmap(mux_base);
    iounmap(dir_base);
    iounmap(data_base);
    device_destroy(gpio_class, devno);
    class_destroy(gpio_class);
    cdev_del(&gpio_cdev);
    unregister_chrdev_region(devno, 1);
    printk("gpio_led driver unloaded\n");
}

module_init(gpio_led_init);
module_exit(gpio_led_exit);
MODULE_LICENSE("GPL");
```

---

## 五、编写 Makefile

```bash
vi /mnt/sdkdisk/gpio_led/Makefile
```

内容（make前必须是Tab键）：

```makefile
KERN_DIR = /mnt/sdkdisk/rk356x-linux/kernel

obj-m := gpio_led_drv.o

all:
	make -C $(KERN_DIR) M=$(PWD) modules ARCH=arm64 CROSS_COMPILE=/mnt/sdkdisk/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

clean:
	make -C $(KERN_DIR) M=$(PWD) modules clean
```

---

## 六、编写测试程序

```bash
vi /mnt/sdkdisk/gpio_led/gpio_led_test.c
```

内容：

```c
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>

#define LED_MAGIC   'L'
#define LED_ON      _IO(LED_MAGIC, 0)
#define LED_OFF     _IO(LED_MAGIC, 1)

int main(int argc, char *argv[])
{
    int fd;

    if (argc != 2) {
        printf("用法: %s <1|0>  (1=点亮, 0=熄灭)\n", argv[0]);
        return -1;
    }

    fd = open("/dev/gpio_led", O_RDWR);
    if (fd < 0) {
        perror("open /dev/gpio_led failed");
        return -1;
    }

    if (argv[1][0] == '1') {
        ioctl(fd, LED_ON);
        printf("LED 点亮\n");
    } else {
        ioctl(fd, LED_OFF);
        printf("LED 熄灭\n");
    }

    close(fd);
    return 0;
}
```

---

## 七、编译驱动和测试程序

```bash
cd /mnt/sdkdisk/gpio_led

# 编译驱动
make

# 交叉编译测试程序
/mnt/sdkdisk/rk356x-linux/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc gpio_led_test.c -o gpio_led_test
```

---

## 八、烧写内核到开发板

### 8.1 将文件传到Windows

```bash
# 挂载共享文件夹
sudo mkdir -p /mnt/hgfs
sudo vmhgfs-fuse .host:/ /mnt/hgfs -o allow_other

# 复制文件
cp /mnt/sdkdisk/rk356x-linux/kernel/boot.img /mnt/hgfs/20251212/
cp /mnt/sdkdisk/gpio_led/gpio_led_drv.ko /mnt/hgfs/20251212/
cp /mnt/sdkdisk/gpio_led/gpio_led_test /mnt/hgfs/20251212/
```

### 8.2 使用RKDevTool烧写

1. 开发板断电，按住 **Loader烧录按键** 上电，进入Loader模式
2. 打开 **RKDevTool.exe**
3. 只勾选第5行 **boot**，路径选择 `boot.img`
4. 勾选 **"强制按地址写"**
5. 点击**执行**，等待烧写完成

---

## 九、连接串口

### 9.1 接线方式

| 串口线颜色 | 接板子引脚 |
|-----------|-----------|
| 绿色（TX） | RX |
| 白色（RX） | TX |
| 黑色（GND） | GND |
| 红色（VCC） | **不接** |

串口引脚位置：板子左下角 **Debug** 标注处。

### 9.2 连接虚拟机

VMware菜单 → 虚拟机 → 可移动设备 → **QinHeng USB Serial** → 连接

### 9.3 打开串口终端

```bash
sudo minicom -D /dev/ttyUSB0 -b 115200
```

---

## 十、传输文件到开发板

### 10.1 板子连接WiFi

```bash
nmcli dev wifi connect "热点名称" password "热点密码"
ifconfig wlan0   # 查看IP地址
```

### 10.2 从虚拟机传文件（SCP）

```bash
scp /mnt/sdkdisk/gpio_led/gpio_led_drv.ko root@<板子IP>:/root/
scp /mnt/sdkdisk/gpio_led/gpio_led_test root@<板子IP>:/root/
```

---

## 十一、在开发板上运行测试

```bash
# 加载驱动
insmod gpio_led_drv.ko

# 确认设备节点
ls /dev/gpio_led

# 添加执行权限
chmod +x gpio_led_test

# 点亮运行灯
./gpio_led_test 1

# 熄灭运行灯
./gpio_led_test 0

# 卸载驱动
rmmod gpio_led_drv
```

---

## 十二、附加题：IOCTL接口说明

IOCTL（Input/Output Control）是Linux驱动里专门用来发送控制命令的接口，比write接口语义更清晰，更适合开关、配置等控制操作。

驱动中定义命令码：
```c
#define LED_MAGIC   'L'
#define LED_ON      _IO(LED_MAGIC, 0)   /* 点亮命令 */
#define LED_OFF     _IO(LED_MAGIC, 1)   /* 熄灭命令 */
```

测试程序中调用：
```c
ioctl(fd, LED_ON);   /* 点亮 */
ioctl(fd, LED_OFF);  /* 熄灭 */
```

---

## 注意事项

1. SDK必须解压到虚拟机**本地磁盘**，不能在共享目录下编译
2. vi编辑器中**不要使用鼠标滚轮**，用键盘方向键移动
3. Makefile中命令行前必须是**Tab键**，不能是空格
4. 烧写时勾选**"强制按地址写"**，只烧boot分区
5. U盘（存放虚拟机磁盘文件）在实验过程中**不能拔出**
6. GPIO0_D5在_H寄存器里对应**bit13**（D组从bit8开始排列，D5=8+5=13）
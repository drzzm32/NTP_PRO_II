#ifndef __JOY_H
#define __JOY_H

#include <stdint.h>

/* ============================================================================
 * NTP PRO II —— 手柄（Game Pad）MCU 侧接口（CH559）
 *
 * 本文件是 NGB-PAD/pad.h 的同款契约，按本项目改名与加宽：
 *   * 关键字 pad -> joy（pad_state_t -> joy_state_t、padSendIfChanged -> ...）
 *   * 模拟轴由 8 位加宽到 16 位（CH559 的 ADC 是 12 位，见下面"模拟轴"）
 *
 * 分工
 * ----
 *   usb.c    USB 设备栈 + 描述符 + 把报告发出去（实现本文件声明的 joy* 函数）
 *   main.c   业务逻辑：读 ADC、扫按键、决定 hat 和轴的值      <- 应用侧
 *
 * 应用侧要做的只有两件事：
 *   1) 更新 g_joy
 *   2) 调 joySendIfChanged()
 *
 * 不要直接读写 usb.c 里的任何变量（JoyReport/Ep1Buffer/UEP1_* 等）。那些是实现
 * 细节，本文件声明的接口就是全部契约。
 * ========================================================================== */


/* ---------------------------------------------------------------- 按键位
 *
 * 每一位对应描述符里的 HID Button 1..15。内核 hid-input.c:759 一带把
 * Button N **固定**映射到下面的 Linux 事件码（code += BTN_GAMEPAD）：
 *
 *   Button  1 -> BTN_SOUTH   (A / 下)      Button  9 -> BTN_TL2    (L2)
 *   Button  2 -> BTN_EAST    (B / 右)      Button 10 -> BTN_TR2    (R2)
 *   Button  3 -> BTN_C                     Button 11 -> BTN_SELECT
 *   Button  4 -> BTN_NORTH   (X / 左)      Button 12 -> BTN_START
 *   Button  5 -> BTN_WEST    (Y / 上)      Button 13 -> BTN_MODE   (MENU)
 *   Button  6 -> BTN_Z                     Button 14 -> BTN_THUMBL
 *   Button  7 -> BTN_TL      (L1)          Button 15 -> BTN_THUMBR
 *   Button  8 -> BTN_TR      (R1)
 *
 * RetroArch 的 udev 驱动再把这些按出现顺序编号成 btn 0..14。
 * 所以"物理 A 键"接 Button 1、"START"接 Button 12、"MENU"接 Button 13。
 *
 * 为什么面键用 1/2/4/5 而不是 1/2/3/4：Linux 的通用映射严格按 usage 编号
 * （drivers/hid/hid-input.c:749  code = (usage->hid - 1)；
 *   :759  gamepad 时 code += BTN_GAMEPAD = 0x130），于是
 *     usage 1→BTN_SOUTH  2→BTN_EAST  3→BTN_C  4→BTN_NORTH  5→BTN_WEST
 * 这些名字是按**位置**（下/右/左/上）定的，与按键上的字母无关。所以要让 Linux
 * 把 X 认成 BTN_NORTH、Y 认成 BTN_WEST，就必须用 4/5；若图省事写成 1/2/3/4，
 * X 会变成 BTN_C、Y 变成 BTN_NORTH —— 直接错位，RetroArch 也就认错了。
 *
 * 若在 Windows 的"游戏控制器"测试里看到 A/B/X/Y 显示成 1/2/3/4：那是 Windows
 * 把"从没被置位的按钮 3"跳过后重新编号的结果，与固件无关。
 * **验收请在板子上用 `evtest` 看 BTN_* 名字**，不要看 Windows 的序号。
 */
#define JOY_BTN_A       0x0001u
#define JOY_BTN_B       0x0002u
#define JOY_BTN_C       0x0004u
#define JOY_BTN_X       0x0008u
#define JOY_BTN_Y       0x0010u
#define JOY_BTN_Z       0x0020u
#define JOY_BTN_L1      0x0040u
#define JOY_BTN_R1      0x0080u
#define JOY_BTN_L2      0x0100u
#define JOY_BTN_R2      0x0200u
#define JOY_BTN_SELECT  0x0400u
#define JOY_BTN_START   0x0800u
#define JOY_BTN_MENU    0x1000u
#define JOY_BTN_L3      0x2000u
#define JOY_BTN_R3      0x4000u


/* ---------------------------------------------------------------- 十字键 hat
 *
 * 重要：hat 的值**不是**"方向编号"，很容易写错。
 *
 * 内核拿到 hat 字段后先算
 *     hat_dir = (value - hat_min) * 8 / (hat_max - hat_min + 1) + 1
 * 再查表（kernel: drivers/hid/hid-input.c:1524 与文件开头的 hid_hat_to_axis）：
 *     hid_hat_to_axis[] = {{0,0},{0,-1},{1,-1},{1,0},{1,1},{0,1},{-1,1},{-1,0},{-1,-1}}
 *
 * 本描述符里 hat 是 Logical 0..7，于是：
 *     value 0 -> hat_dir 1 -> ( 0,-1) 上
 *     value 1 -> hat_dir 2 -> ( 1,-1) 右上
 *     value 2 -> hat_dir 3 -> ( 1, 0) 右
 *     value 3 -> hat_dir 4 -> ( 1, 1) 右下
 *     value 4 -> hat_dir 5 -> ( 0, 1) 下
 *     value 5 -> hat_dir 6 -> (-1, 1) 左下
 *     value 6 -> hat_dir 7 -> (-1, 0) 左
 *     value 7 -> hat_dir 8 -> (-1,-1) 左上
 *     value 8 -> hat_dir 9 -> 越界归 0 -> 居中
 *     value 15-> hat_dir 16-> 越界归 0 -> 居中
 *
 * 也就是说**任何 >7 的值都是居中**。写错成"0 = 居中"会表现为方向键永久按住上。
 * 注意 Y 轴正方向是**下**（evdev 约定）。
 */
#define JOY_HAT_UP          0x00u
#define JOY_HAT_UP_RIGHT    0x01u
#define JOY_HAT_RIGHT       0x02u
#define JOY_HAT_DOWN_RIGHT  0x03u
#define JOY_HAT_DOWN        0x04u
#define JOY_HAT_DOWN_LEFT   0x05u
#define JOY_HAT_LEFT        0x06u
#define JOY_HAT_UP_LEFT     0x07u
#define JOY_HAT_CENTER      0x0Fu   /* 写 0x08 也等效，两者都会被判为居中 */


/* ---------------------------------------------------------------- 模拟轴
 *
 * 四个轴 X / Y / Z / Rz，各**有符号 16 位字段**（报告里占 2 字节、小端补码），
 * 逻辑范围 **-1023 .. +1023**，**中点是 0**。
 *
 * 为什么是 -1023..1023：CH559 的 ADC 名义上 12 位，但本项目的有效位数只有 11 位
 * （adcGet() 返回后按 & 0x7FF 取值，即 0..2047，中点 1023）。也就是说一次采样的
 * 满量程对称区间恰好是 -1023..+1023 —— 描述符的逻辑范围**照着硬件的真实分辨率
 * 写**，应用侧把读数减去中点就能原样上报，不需要任何缩放或移位，主机拿到的就是
 * 真实的 ADC 计数，不会出现"假精度"。
 *
 * 为什么字段仍写 16 位而不是 11 位：HID 的 Report Size 只有 1/2/4/8/16/32 这几档，
 * 没有 11 位可选。11 位数据放进 16 位字段，高位是符号扩展，主机按逻辑范围
 * -1023..1023 解读——多出来的 5 位既不会被解读错，也不影响什么（载荷由 7 字节变成
 * 11 字节，但中断端点仍然是每个 bInterval 一帧）。
 *
 * 若哪天换用 12 位满量程（0..4095，中点 2047），只需把描述符里 Logical
 * Minimum/Maximum 改成 -2047..+2047，并把下面的 JOY_AXIS_MIN/MAX 同步改掉，
 * 本文件与 usb.c 其余部分都不用动。
 *
 * **没接的通道必须固定填 JOY_AXIS_CENTER（0）**。悬空的 ADC 引脚会读到随机值，
 * 主机侧表现为"摇杆自己乱飘"，很多游戏会因此完全不可操作。
 */
#define JOY_AXIS_MIN    (-1023)
#define JOY_AXIS_MAX    (1023)
#define JOY_AXIS_CENTER (0)


/* ---------------------------------------------------------------- 一帧状态
 *
 * 业务层只写这个结构，USB 层只读它。
 */
typedef struct {
    uint16_t buttons;   /* JOY_BTN_* 的按位或 */
    uint8_t  hat;       /* JOY_HAT_* 之一 */
    int16_t  x;         /* JOY_AXIS_MIN..JOY_AXIS_MAX，中点 0 */
    int16_t  y;
    int16_t  z;
    int16_t  rz;
} joy_state_t;

extern joy_state_t g_joy;


/* ---------------------------------------------------------------- 业务层调用
 *
 * 这几个由 usb.c 实现。
 */

/* 主机已完成 SET_CONFIGURATION，可以发报告了。
 * 在这之前调用 joySend* 是**无害的空操作**，所以主循环可以无脑调。 */
__bit joyReady(void);

/* 立刻把 g_joy 打包成一帧发出去（不等"内容是否变化"）。 */
void joySend(void);

/* 只有 g_joy 和上一帧不同才发。返回是否真的发了。
 * 手柄正常应该用这个：主机按 bInterval 轮询，重复发同样的内容是浪费带宽，
 * 但**必须**在内容变化时立刻发，否则按键会延迟到下一次变化才生效。 */
uint8_t joySendIfChanged(void);

/* 标记"缓存里的上一帧已失效"，强制下一次 joySendIfChanged() 一定发送。
 * 主机刚配置好、或刚做完一次模式/身份切换时调用。 */
void joyInvalidate(void);


/* ---------------------------------------------------------------- 可选：主机 -> 设备
 *
 * 走描述符里那个 vendor 自定义接口（接口1，Report ID 0xAA 输入 / 0x55 输出），
 * 可以拿来下发配置（比如标定中点）或做调试通道。不用的话完全忽略即可。
 */
__bit    joyHasHostData(void);      /* 主机有数据来了 */
uint8_t  joyHostData(uint8_t index);/* 读第 index 字节 */
void     joyRequestHostData(void);  /* 请求接收（上电时调一次） */


/* ---------------------------------------------------------------- 可选：调试文本
 *
 * 通过 vendor 接口把一小段 ASCII 发给主机，PC 侧可直接读（不需要串口）。
 * 内容会被截断到 HID_BUF_SIZE(42) 字节以内。
 */
void joyPrint(const char *str);


#endif /* __JOY_H */

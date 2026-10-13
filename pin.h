#ifndef __PIN_H
#define __PIN_H

#include "ch559.h"

/* ============================================================================
 * NTP PRO II R2 面板引脚定义
 *
 * 面板 I/O 一览（编号与丝印/原理图一致）：
 *
 *   A0 电流采样   AIN_0   电流采样（ISEN）
 *   A1 功率盘     AIN_1   模拟 -> pwr
 *   A2 方向柄     AIN_2   模拟，三选一开关 -> 前进/中立/后退
 *   A3 右线性手柄 AIN_3   模拟 -> v_off（本身就是 0~2047mV）
 *
 *   P0 电源指示   P3.0    初始化完成后常亮
 *   P1 PWM输出指示 P3.1   跟 K2（PWM 输出控制）
 *   P2 正向指示   P3.2
 *   P3 中位指示   P3.3    急停期间这三只全灭
 *   P4 反向指示   P3.4
 *
 *   K0 板载按键   P0.0    预留，未定义功能
 *   K1 急停       P0.1    自复位；锁存，方向柄回中位才解除
 *   K2 PWM输出控制 P0.2   切换开关（有状态）
 *   K3 压控输出控制 P0.3  切换开关（有状态）
 *   K4 PWM频率 −  P0.4    自复位
 *   K5 PWM频率 +  P0.5    自复位
 *   K6 / K7       P0.6/P0.7   原理图上预留，**本板已让给 OLED 的 CS/DC**，不可用
 *
 * 约定（两处极性都只在下面这两个宏里体现，实物相反时只改这里）：
 *   开关/按键：读入 0 = 按下/闭合
 *   指示灯   ：置 1 = 亮
 * ========================================================================== */

#define SW_ACTIVE(x)    ((x) == 0)      /* 开关/按键"按下/打开"时的读数 */
#define LED_ON          1
#define LED_OFF         0

// IIC（OLED 走 SPI0，见 oled.c）
#define SCL     P20
#define SDA     P21

/* 输出级：电机 H 桥 与 PWM1/PWM2 **共用 P4.3 / P4.5**。
   ch559.h 的 bPWM1_PIN_X：0 = PWM 在 P2.4/P2.5，1 = PWM 在 P4.3/P4.5。
   也就是说这两个脚要么当普通 GPIO 驱动 H 桥，要么交给 PWM —— 二选一，
   由 K2（PWM 输出控制）决定（见 main.c 的 setStage）。 */
#define MOT_F   P43     // 正向，或 PWM1
#define MOT_R   P45     // 反向，或 PWM2

/* 开关。★ K0 是板载按键、预留，不参与面板逻辑；K6/K7 已被 OLED 占用，见文件头。
   sys.c 的 sysPinConfig() 里 `P0_DIR = 0x3F` 正好覆盖 P0.0~P0.5 = K0~K5。 */
#define K0      P00     // 板载按键，预留
#define K1      P01     // 急停（自复位）
#define K2      P02     // PWM 输出控制（切换开关）
#define K3      P03     // 压控输出控制（切换开关）
#define K4      P04     // PWM 频率 −（自复位）
#define K5      P05     // PWM 频率 +（自复位）

/* 模拟通道。CH559 的 AIN0~AIN7 就是 P1.0~P1.7，但只有 AIN0~AIN4 = P1.0~P1.4
   能用：P1.5/P1.6/P1.7 是 OLED 的 MOSI / RST / SCK。sys.c 的 adcGet() 里那句
   `channel %= 5` 就是为此，所以**不要再定义 AIN5~AIN7**。 */
#define ISEN    0       // A0 电流采样
#define AIN_1   1       // A1 功率盘      -> pwr
#define AIN_2   2       // A2 方向柄      -> 三选一
#define AIN_3   3       // A3 右线性手柄  -> v_off
#define AIN_4   4       // 预留

/* 指示灯（P3.0~P3.4，置 1 亮） */
#define LED_PWR     P30     // 电源指示：初始化完成后常亮
#define LED_PWM     P31     // PWM 输出指示：K2 打开时亮
#define LED_FWD     P32     // 正向指示
#define LED_NEUT    P33     // 中位指示
#define LED_REV     P34     // 反向指示
// P35~P37 预留
// 注：P4.7 上原来还有一颗 LED，R2 不再使用 —— sys.c 的 sysPinConfig() 里
//     已经把它从 P4_DIR 里去掉（保持输入态，灯自然不亮）。

#endif

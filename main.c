/* ============================================================================
 * NTP PRO II (R2 面板) —— 应用层（CH559）
 *
 * 面板 I/O（引脚定义已在 pin.h 里合并说明）：
 *   A1 功率盘       AIN_1  -> pwr
 *   A2 方向柄       AIN_2  -> 三选一开关，**绝对位置**：前进/中立/后退
 *   A3 右线性手柄   AIN_3  -> v_off
 *   P0..P4 五个指示灯（置 1 亮）：电源 / PWM 输出 / 正向 / 中位 / 反向
 *   K1..K5 五个开关（读入 0 为按下）：急停 / PWM 输出控制 / 压控输出控制 /
 *                                     PWM 频率− / PWM 频率+（后两个自复位）
 *   ★ K0 是板载按键（预留，本文件不使用）；K6/K7 原理图预留但已让给 OLED 的 CS/DC。
 *
 * 两种形态，**上电判定一次**（硬件只接 PC 或 USB PD 电源，换形态必须断电）：
 *
 *   USB 在手 → 标准手柄：**只上报 HID，不驱动任何输出级**
 *              X = A1 功率盘、Y = A3 右线性手柄、hat = A2 方向柄（上/中/下）、
 *              按钮 = K1~K5 共 5 个。
 *              指示灯仍然随开关状态变化（见 ledPanel）。
 *   USB 不在 → 独立控制器：按 K2/K3 决定输出级与整定电压。
 *
 * 输出级 —— K2/K3 是**切换开关**（有状态），所以每轮读电平，不是上电锁存：
 *
 *   K2 = PWM 输出控制：
 *        打开 → PWM 输出（P4.3/P4.5 交给 PWM1/PWM2，占空比取功率盘 pwr）
 *        关闭 → 电机 H 桥（同样两个脚，当普通 GPIO 按方向拨）
 *        ↑ 两者共用引脚，所以切换时要重配一次 PIN_FUNC，见 setStage()
 *
 *   K3 = 压控输出控制，只决定整定电压：
 *        K2 开, K3 关 → 额定 12V（右线性手柄 v_off 无效）
 *        K2 开, K3 开 → v_off * TRACK_MV_PER_COUNT
 *        K2 关, K3 开 → pwr  * TRACK_MV_PER_COUNT + v_off
 *        K2 关, K3 关 → v_off * TRACK_MV_PER_COUNT
 *
 * OLED（256x64 / 4 位灰度）两种形态各一屏，见 oledShowLocal() / oledShowUsb()。
 * 第一行是标题（灰底），三行读数在下面：
 *   独立形态：Vset 设置电压 / Fpwm PWM 频率 / Duty 占空比 / Voff 偏置电压(mV)
 *             / Isen 电流 / Dir 方向
 *   USB 形态：USB mode + Isen 电流 / X Y 两个轴 + Dir 方向 / Btn 按下的开关
 *   ⚠️ 字库只有 ASCII（font.c 的索引是 c - ' '），所以标签只能用英文缩写。
 *   ⚠️ 方向**单独占一格**打印，因为它来自 AIN_2 的三选一开关，是这一屏里唯一要猜
 *      硬件的地方，也是目前最可疑的一路；细节见 oledShowLocal() 上面的说明。
 *
 * 急停（K1，只在独立形态下有效，USB 形态没有这个设计）：**锁存** —— 按下即置位，
 * 之后必须等方向柄回到中立位才解除；急停期间输出按中立处理，且三只方向指示灯全灭。
 *
 * 判定"接没接主机"的手段是"等 SET_CONFIGURATION"，而不是"看总线上有没有电平活动"：
 * PD / QC 一类充电器会在 D+/D- 上做 BC1.2 / QC 传统握手（拉固定电平、甚至把 D+/D-
 * 短起来），只看电平会把充电器误判成主机。SET_CONFIGURATION 只有真主机发得出来。
 * ========================================================================== */

#include "ch559.h"

#include <stdio.h>
#include <string.h>

#include "sys.h"
#include "pin.h"
#include "usb.h"
#include "joy.h"
#include "dac.h"
#include "oled.h"

/* 中断向量里的名字必须和 usb.c / sys.c 里的定义完全一致 */
void __usb_interrupt()  __interrupt (INT_NO_USB)  __using (1);
void __pwm1_interrupt() __interrupt (INT_NO_PWM1) __using (2);

/* 调试文本 sprintf 的目标缓冲。**故意放文件作用域**：8051 的栈很小（SDCC 把它分配
   在 iRAM，本工程 --iram-size 256），把 32 字节摊在栈上不划算。 */
char buff[32];

/* 上电时定下来、之后只读（放文件作用域一是省栈，二是后面接 OLED 显示时能直接读） */
__bit USB_HOST = false;     /* true = 接在 PC 上（手柄形态） */

/* USB 层只读这个结构，业务层负责写。定义放在应用侧，语义更清楚：
 * "这一帧要报什么"是应用的状态，不是 USB 的状态。 */
joy_state_t g_joy;


/* ============================================================================
 * 量纲与常量
 *
 * ADC 名义 12 位，但本项目只取低 **11** 位（& ADC_MASK → 0..2047），中点是 1023。
 * 三个模拟量都是这个量程：A1 功率盘、A2 方向柄、A3 右线性手柄（A0 是电流采样）。
 * ========================================================================== */

#define ADC_MASK        0x07FFu     /* 有效位掩码：11 位 */
#define ADC_FULL        2047u       /* 11 位满量程计数，用于 mV 换算 */
#define ADC_MID         1023u       /* 上述区间的中点 */
#define ADC_REF_MV      3300u       /* ADC 参考电压 3.3V */

/* 方向柄（三选一开关，走 A2）：靠 ADC 分档读出位置。
   0..699 → -1 后退，700..1399 → 0 中立，1400..2047 → +1 前进 */
#define LEVER_STEP      700u

/* 电流采样（A0/ISEN）换算：零电流时输出 1650 mV，灵敏度 132 mV/A，硬件分压 4 倍。
   ⚠️ 这三个常量**不能带 u 后缀**：一带上整个表达式就从有符号除法变成无符号除法，
   负电流（充电）算出来的值就变了。
   ⚠️ 末尾那个"除以 4"的方向按原理图核一下：取样若是 4 倍**增益**，这里该乘 4。 */
#define MV_PER_V        1000
#define ISEN_ZERO_MV    1650
#define ISEN_MV_PER_A   132
#define ISEN_DIV        4

/* 轨道电压换算：A1/A3 都是 0..2047，每计数算 6 mV，满量程 ≈ 12.28 V。
   voltSet() 收毫伏，内部再映射到 DAC5571 的 0..255。 */
#define TRACK_MV_PER_COUNT  6u
#define TRACK_MV_RATED      12000u  /* K2 开、K3 关时的额定输出电压（12V） */

/* 0..2047 的 ADC 读数 → 0..255 的 PWM 占空比 */
#define PWM_MAX_DUTY    255u
#define ADC_TO_DUTY(v)  ((uint8_t) ((v) >> 3))

/* 手柄按键位：K1~K5 对应前 5 个按钮。
   方向**不再**占按钮位 —— 它报在 hat 里（旧固件用 bit4/bit5 表示方向，已取消）。 */
#define BTN_K1          0x01u
#define BTN_K2          0x02u
#define BTN_K3          0x04u
#define BTN_K4          0x08u
#define BTN_K5          0x10u

/* 节奏 */
#define DBG_REFRESH_MS  100u        /* 无输入变化时，刷新调试文本的间隔 */
#define IDLE_POLL_MS    20u         /* 独立控制器模式下的轮询间隔 */

/* 形态判定的等待窗口：初始化时最多等这么久，等主机发 SET_CONFIGURATION。
   ⚠️ 这个值有两个含义：接 USB PD 电源时它就是"上电后多等多久才开始驱动输出级"；
      接 PC 时它是"主机慢到多久会被误判成独立控制器"的界线。2000ms 对 PC 一侧
      非常宽松（通常远小于 500ms），板子上实测后可调。 */
#define USB_MODE_TIMEOUT_MS 2000u
#define USB_MODE_POLL_MS    50u

/* PWM 频率档位表：f = FREQ_SYS / PWM_CK_SE / (PWM_CYCLE + 1)
   K5 往上一档、K4 往下一档，到两端就停住；**掉电不保存**（普通变量）。
   出厂档 PWM_FREQ_DEFAULT = (2, 255) → 56MHz / 2 / 256 = 109.4kHz，
   与原 pwmInit() 里写死的 PWM_CK_SE=2 / PWM_CYCLE=255 一致。
   ⚠️ 下面这张表是按"覆盖约 3.4k~219kHz、尽量保住占空比级数"编的；
      要改范围或步进，只改这两行表和默认档即可。
   屏上显示的频率来自 pwm_dhz[]，那份是初始化时按这两张表算出来的，
   所以改表不会出现"显示和实际对不上"。 */
#define PWM_FREQ_DEFAULT    10
#define PWM_FREQ_STEPS      13
const static uint8_t pwm_ckse_tab[PWM_FREQ_STEPS] = {
    64, 48, 32, 24, 16, 12,  8,  6,  4,  3,  2,  2,  1
};
const static uint8_t pwm_cycle_tab[PWM_FREQ_STEPS] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 191, 255
};
uint8_t  pwm_freq_idx = PWM_FREQ_DEFAULT;           /* 当前档位 */
uint16_t pwm_dhz[PWM_FREQ_STEPS];                   /* 各档频率，单位 0.1kHz */

/* OLED 刷屏节拍。没有空闲定时器可用，就在主循环每次延时上累加毫秒 ——
   刷屏只要 5Hz，这个精度足够；间隔用无符号减法判，回绕不会出错。 */
#define OLED_REFRESH_MS 200u
uint16_t ms_tick   = 0;
uint16_t oled_last = 0;


/* ---------------------------------------------------------------- 形态判定
 *
 * 上电时调一次，返回是否接在 PC 上。
 */
static uint8_t waitUsbHost(void) {
    uint16_t waited = 0;
    while (waited < USB_MODE_TIMEOUT_MS) {
        if (joyReady()) return 1;
        delay(USB_MODE_POLL_MS);
        waited += USB_MODE_POLL_MS;
    }
    return joyReady();          /* 最后一次机会：刚好在超时边界上配置完 */
}


/* ---------------------------------------------------------------- 模拟量换算 */

/* A0/ISEN 的 ADC 读数 → 电流（mA）。正 = 放电，负 = 充电。
   ⚠️ 中间量**必须走 32 位**：ad_val 最大 2047，2047 × 3300 ≈ 6.75e6，16 位放不下。
   （原来的写法就是 16 位相乘，ad_val > 19 即溢出，算出来的电流全是垃圾。） */
static int16_t currentMa(uint16_t ad_val) {
    int32_t mv = ((int32_t) ad_val * ADC_REF_MV) / ADC_FULL;    /* 先换算成 mV */
    return (int16_t) (((mv - ISEN_ZERO_MV) * MV_PER_V) / ISEN_MV_PER_A / ISEN_DIV);
}

/* 整定输出电压（mV）—— K2/K3 四种组合各一条，见文件头。
   pwm/vctl 传的是"开关是否打开"（1 = 打开）。 */
static uint16_t setpointMv(uint8_t pwm, uint8_t vctl, uint16_t v_off, uint16_t pwr) {
    if (pwm)                                     /* PWM 输出形态 */
        return vctl ? (uint16_t) (v_off * TRACK_MV_PER_COUNT)
                  : (uint16_t) TRACK_MV_RATED;  /* 额定 12V，v_off 无效 */
    return vctl ? (uint16_t) (pwr * TRACK_MV_PER_COUNT + v_off)
              : (uint16_t) (v_off * TRACK_MV_PER_COUNT);
}


/* ---------------------------------------------------------------- 输出级 */

/* 当前是不是 PWM 形态。初值 0 = 电机 H 桥，因为 sysPinConfig() 之后这两个脚就是
   普通 GPIO、main() 开始处也已经把两端都置 0 了。 */
static __bit pwm_stage = 0;

/* 切换输出级形态。PWM 与电机 H 桥**共用 P4.3/P4.5**（见 pin.h 与 ch559.h 的
   bPWM1_PIN_X），所以切换时要重配一次引脚 —— 只在形态变化时做，不要每轮都动。 */
static void setStage(uint8_t pwm) {
    if (pwm == pwm_stage) return;
    pwm_stage = pwm;

    if (pwm) {
        pwmInit();                                  /* 其中会置 PIN_FUNC |= bPWM1_PIN_X */
        PWM_CK_SE = pwm_ckse_tab[pwm_freq_idx];
        PWM_CYCLE = pwm_cycle_tab[pwm_freq_idx];
    } else {
        PWM_DATA  = 0;                              /* 先停输出再让引脚 */
        PWM_DATA2 = 0;
        PWM_CTRL &= ~(bPWM_OUT_EN | bPWM2_OUT_EN);  /* 关掉 PWM 输出使能 */
        PIN_FUNC &= ~bPWM1_PIN_X;                   /* PWM 改去 P2.4/P2.5，P4.3/P4.5 让回 GPIO */
        IE_PWM1 = 0;
    }
}

/* K4/K5 调 PWM 频率（自复位开关，按一次走一档）。只有 PWM 形态下才动寄存器。 */
static void freqStep(uint8_t up) {
    if (up) {
        if (pwm_freq_idx + 1u < PWM_FREQ_STEPS) pwm_freq_idx++;
    } else {
        if (pwm_freq_idx > 0u) pwm_freq_idx--;
    }
    if (pwm_stage) {
        PWM_CK_SE = pwm_ckse_tab[pwm_freq_idx];
        PWM_CYCLE = pwm_cycle_tab[pwm_freq_idx];
    }
}

/* 按挡位拨电机 H 桥（K2 关闭时的形态）。
   中立 = 两端都断开（惯性滑行），不刹车。 */
static void motorByDir(int8_t dir) {
    switch (dir) {
        case -1:  MOT_F = 0; MOT_R = 1; break;
        case  0:  MOT_F = 0; MOT_R = 0; break;
        case  1:  MOT_F = 1; MOT_R = 0; break;
        default:  MOT_F = 1; MOT_R = 1; break;   /* 挡位被钳在 ±1，正常到不了这里 */
    }
}

/* 按挡位拨两路 PWM（K2 打开时的形态）。duty 是 **0..255**，调用方负责换算。 */
static void pwmByDir(int8_t dir, uint8_t duty) {
    switch (dir) {
        case -1:  pwmSet(1, 0);            pwmSet(2, duty); break;
        case  0:  pwmSet(1, 0);            pwmSet(2, 0);    break;
        case  1:  pwmSet(1, duty);         pwmSet(2, 0);    break;
        default:  pwmSet(1, PWM_MAX_DUTY); pwmSet(2, PWM_MAX_DUTY); break;
    }
}


/* ---------------------------------------------------------------- 面板指示灯
 *
 * 直接跟开关状态走，不受"是否驱动输出级"影响，所以两种形态下行为相同。
 *   LED_PWM  跟 K2（PWM 输出控制）
 *   LED_FWD / LED_NEUT / LED_REV 跟方向柄的位置
 * 置 1 亮、置 0 灭。
 *
 * 例外是急停：触发期间**三只方向灯全灭**（既不亮前进也不亮中立），
 * 只留 LED_PWM 继续报 K2 —— 这样一眼能看出"不是在中立，是急停了"。
 * USB 形态没有急停，estop 恒为 0。
 */
static void ledPanel(uint8_t k2_on, int8_t dir, uint8_t estop) {
    LED_PWM  = k2_on ? LED_ON : LED_OFF;

    if (estop) {
        LED_FWD  = LED_OFF;
        LED_NEUT = LED_OFF;
        LED_REV  = LED_OFF;
    } else {
        LED_FWD  = (dir ==  1) ? LED_ON : LED_OFF;
        LED_NEUT = (dir ==  0) ? LED_ON : LED_OFF;
        LED_REV  = (dir == -1) ? LED_ON : LED_OFF;
    }
}


/* ---------------------------------------------------------------- OLED 屏
 *
 * 256x64 / 4 位灰度，用 8x16 字库（fid = 1）排 4 行、每行 32 字符。
 * **标题占第一行**（y=0，开机时画一次），三行读数从第二行起（y=16/32/48）。
 *
 *   独立形态：                                   USB 形态：
 *     y= 0        NTP PRO II                            NTP PRO II
 *     y=16  Vset 12.00V  Fpwm 109.4kHz            USB mode   Isen  1250mA
 *     y=32  Duty  100%   Voff 2047mV              X  1023 Y -1023  Dir FWD
 *     y=48  Isen   1250mA  Dir FWD                Btn K1--K4--
 *
 * 方向单独占一格（Dir）：它来自 AIN_2 的三选一开关，是这一屏里**唯一要猜硬件**
 * 的地方，必须能在屏上直接看到，而不是只靠指示灯推断。详见 oledShowLocal()。
 *
 * 三个关键约束：
 *   1) 字库只有 ASCII（font.c 的索引是 c - ' '），标签只能用英文缩写。
 *   2) 所有字段都用**固定宽度**格式化（%3u / %4u / %5u / %6d / %02u）。这样覆盖写
 *      同一个位置时不会留下上一帧的尾巴 —— 否则数值从 3 位变 2 位就会拖个残影。
 *      电流和轴值特意用 %6d 而不是 %5d：它们能到 -3125 / -1023，负号会把 %5d 占满，
 *      和前面的标签黏成 "Isen-3125mA"。
 *   3) 方向和按钮那两处是手工拼接，不用 %s（会把 printf 的字符串支持也链进来）。
 *
 * 刷新策略：每行先格式化再和上一次画上去的比，**没变就一个字节都不发**。
 * 一次全屏重画（三行共几十个字符）量级是几毫秒 —— 开销主要不在 SPI（8MHz、
 * 也就 4KB），而在 oledBinary() 逐列逐行拼 oled_buffer 的那几千次循环。所以外面
 * 限到 5Hz（见 OLED_REFRESH_MS），平均占用几个百分点。
 */
static char oled_line[3][28];       /* 这一帧的三行读数 */
static char oled_prev[3][28];       /* 上一次真正画上去的，用来判"变了没" */

#define OLED_TITLE_Y    0           /* 标题在第一行 */
#define OLED_LINE0_Y    16          /* 读数从第二行起 */
#define OLED_LINE_DY    16

/* 方向的三字母写法，下标用 dir + 1（-1/0/+1 → 0/1/2）。急停时另外显示 "STP" */
const static char oled_dir_txt[3][4] = { "REV", "NEU", "FWD" };

/* 把变了的那几行推上屏。两种形态共用。 */
static void oledFlush(void) {
    uint8_t i;
    for (i = 0; i < 3; i++) {
        if (strcmp(oled_line[i], oled_prev[i]) != 0) {
            oledString(0, (uint16_t) (OLED_LINE0_Y + i * OLED_LINE_DY),
                       1, 0x0F, oled_line[i]);
            strcpy(oled_prev[i], oled_line[i]);
        }
    }
}

/* 独立形态：显示输出级的整定量、实测电流，以及**方向**。
 *
 * ⚠️ 方向来自 AIN_2 的三选一开关，是这一屏里唯一一处"要猜硬件"的地方：主循环用
 *    ad_val / LEVER_STEP 把它分成三段（0~699 / 700~1399 / 1400~2047）。
 *    如果开关某个位置正好落在 700 或 1400 这个分档点附近，ADC 抖动会让读数在相邻
 *    两档之间跳，而主循环要求"连续两次一致才认"，于是 dir 永远定不下来、看着像没
 *    方向。屏上把 Dir 和三字母单独打出来，就是为了能一眼分辨"是解码错还是分压不对"。
 *    要看更细的原始读数，走 vendor 接口那行调试文本，里面有 AIN_2 的原值（A%d）。
 *
 * voff_mv 是**偏置电压本身**（0~2047mV，就是 A3 的读数），不是乘以 6 之后的
 * 轨道电压 —— 后者是 setpointMv() 里把整定值放大到满量程时才算的。
 */
static void oledShowLocal(uint16_t vset_mv, uint16_t dhz, uint8_t duty_pct,
                          uint16_t voff_mv, int16_t isen_ma,
                          int8_t dir_out, uint8_t estop) {
    sprintf(oled_line[0], "Vset%3u.%02uV  Fpwm%4u.%1ukHz",
            vset_mv / 1000u, (vset_mv % 1000u) / 10u, dhz / 10u, dhz % 10u);
    sprintf(oled_line[1], "Duty%4u%%   Voff%5umV", duty_pct, voff_mv);
    /* 这一行是 Isen + Dir 两格，Dir 手工拼在固定偏移上（那里一共 18 字符） */
    sprintf(oled_line[2], "Isen%6dmA  Dir ", isen_ma);
    strcpy(oled_line[2] + 18, estop ? "STP" : oled_dir_txt[dir_out + 1]);
    oledFlush();
}

/* USB 形态：输出级没在驱动，改显示输入状态 —— 两个轴、方向、按下的开关，外加
 * 仍然可测的电流。 */
static void oledShowUsb(int16_t ax, int16_t ay, int8_t dir, uint8_t btn,
                        int16_t isen_ma) {
    uint8_t i;

    sprintf(oled_line[0], "USB mode   Isen%6dmA", isen_ma);
    /* %6d 而不是 %5d：轴值能到 ±1023，负号会把 %5d 占满，和前面的 X/Y 黏在一起 */
    sprintf(oled_line[1], "X%6d Y%6d  Dir ", ax, ay);   /* 到此 21 字符 */
    strcpy(oled_line[1] + 21, oled_dir_txt[dir + 1]);

    strcpy(oled_line[2], "Btn ");                       /* 之后 5 组，每组 2 字符 */
    for (i = 0; i < 5; i++) {
        uint8_t on = (btn & (1u << i)) ? 1 : 0;
        oled_line[2][4 + i * 2]     = on ? 'K' : '-';
        oled_line[2][4 + i * 2 + 1] = on ? (char) ('1' + i) : '-';
    }
    oled_line[2][4 + 5 * 2] = '\0';

    oledFlush();
}


void main() {
    /* ---------------------------------------------------------------- 上电初始化 */

    /* 先把输出级两端松开，避免上电瞬间乱动（这时 P4.3/P4.5 已是输出） */
    MOT_F = 0; MOT_R = 0;

    sysPinConfig();
    sysClockConfig();
    delay(10);

    oledInit();
    oledLogo();                 /* 开机 logo，停 1 秒给人看 */
    delay(1000);

    adcInit();
    dacInit();

    /* g_joy 初值。hat **必须显式写居中**（JOY_HAT_CENTER=0x0F，不是 0；写 0 等于
       "方向键永久按住上"），四轴填中点；否则主机配置完成后收到的第一帧会是
       "方向键按住上 + 摇杆打到左下"。 */
    g_joy.buttons = 0;
    g_joy.hat     = JOY_HAT_CENTER;
    g_joy.x       = JOY_AXIS_CENTER;
    g_joy.y       = JOY_AXIS_CENTER;
    g_joy.z       = JOY_AXIS_CENTER;
    g_joy.rz      = JOY_AXIS_CENTER;

    /* 各档 PWM 频率算一次（屏上要用）。在这里算而不是手写第三张表，是为了避免
       和上面两张表对不上；只做 13 次 32 位除法，无所谓。 */
    {
        uint8_t i;
        for (i = 0; i < PWM_FREQ_STEPS; i++) {
            uint32_t f = (uint32_t) FREQ_SYS
                       / (uint32_t) pwm_ckse_tab[i]
                       / ((uint32_t) pwm_cycle_tab[i] + 1u);
            pwm_dhz[i] = (uint16_t) (f / 100u);         /* 0.1kHz */
        }
    }

    /* 默认档的 PWM 频率先写进去（此刻 PWM 还没使能，写寄存器无害）；
       真正的输出级形态由 K2 在第一次进入独立形态时通过 setStage() 定下来。 */
    PWM_CK_SE = pwm_ckse_tab[PWM_FREQ_DEFAULT];
    PWM_CYCLE = pwm_cycle_tab[PWM_FREQ_DEFAULT];

    usbDevInit();
    EA = 1;                     /* 中断全开：USB 和 PWM 的中断从这里开始服务 */

    /* 标题画在**第一行**（灰底 0x8F 强调），下面三行留给读数，互不干扰。
       开机画一次就够了 —— oledFlush() 只刷那三行，不会把标题冲掉。 */
    oledClear(0x0);
    oledStringC(128, OLED_TITLE_Y, 1, 0x8F, " NTP PRO II ");

    /* 形态判定：接在 PC 上还是独立控制器。这一次判定贯穿整个供电周期。 */
    USB_HOST = waitUsbHost();

    /* 初始化完成，点亮电源指示 */
    LED_PWR = LED_ON;

    /* 让"上一帧"缓存失效，这样主机 SET_CONFIGURATION 之后的第一帧必定发出
       （原来这里是 releaseAllKey() + pushKeyData()，效果相同）。独立形态下这两句
       是无害空操作。 */
    joyInvalidate();
    joyRequestHostData();

    /* ------------------------------------------------- 循环状态（跨轮保留） */
    int8_t  dir      = 0;       /* 方向：-1 后退 / 0 中立 / 1 前进（方向柄的绝对位置） */
    int8_t  dir_raw  = 0;       /* 方向柄这一轮的原始分档值 */
    int8_t  dir_next = 0;       /* 待确认的新档位（连续两次一致才认，滤抖动） */
    uint8_t k4_held  = 0;       /* K4/K5 是自复位开关，做边沿检测 */
    uint8_t k5_held  = 0;

    /* ------------------------------------------------- 每轮刷新的采样值 */
    uint16_t ad_val  = 0;       /* ADC 原始读数（0..2047），多路复用 */
    uint16_t pwr     = 0;       /* A1 功率盘 */
    uint16_t v_off   = 0;       /* A3 右线性手柄，本身就是 0~2047mV 的电压 */
    uint16_t vout    = 0;       /* 整定电压（mV）—— 屏上的 Vset */
    uint8_t  duty    = 0;       /* PWM 占空比 0..255（屏上换算成百分比） */
    int16_t  isen    = 0;       /* 实测电流（mA） */
    uint8_t  estop   = 0;       /* 急停标志：锁存，方向柄回中立才清（仅独立形态） */
    int8_t   dir_out = 0;       /* 实际拿去驱动输出的方向（急停期间恒为中立） */

    while (1) {
        uint8_t k1_on, k2_on, k3_on, btn_now;

        /* ------------------------------------------------------------ 采集
         * adcGet() 是**单通道复用**的：读一次换一路，所以下面四行的顺序就是扫描
         * 顺序，不要合并或提前。
         */
        isen   = currentMa(adcGet(ISEN) & ADC_MASK);    /* A0 电流采样 */
        pwr    = adcGet(AIN_1) & ADC_MASK;              /* A1 功率盘 */
        ad_val = adcGet(AIN_2) & ADC_MASK;              /* A2 方向柄 */
        v_off  = adcGet(AIN_3) & ADC_MASK;              /* A3 右线性手柄 */

        /* -------------------------------------------------- 方向柄解码
         * A2 是**三选一开关**：拨到哪停在哪，直接取绝对位置。
         * 连续两次读到同一个新值才认下来，滤掉机械抖动与 ADC 毛刺。
         * （旧代码是给自复位拨杆写的"步进"语义 —— 用在绝对开关上永远回不了中立。）
         */
        dir_raw = (int8_t) (ad_val / LEVER_STEP) - 1;
        if (dir_raw == dir) {
            dir_next = dir;                 /* 已经在目标位置，清掉待确认值 */
        } else if (dir_raw == dir_next) {
            dir = dir_raw;                  /* 连续两次一致，认下来 */
        } else {
            dir_next = dir_raw;             /* 第一次见到，先记下 */
        }

        /* -------------------------------------------------- 开关（读电平）
         * K1~K5 都是 0 为按下。K2/K3 是切换开关，有状态；K1/K4/K5 是自复位。
         * 顺带拼出手柄上报用的按钮位图（K1~K5 → bit0~bit4），屏上也要用。 */
        k1_on = SW_ACTIVE(K1) ? 1 : 0;
        k2_on = SW_ACTIVE(K2) ? 1 : 0;
        k3_on = SW_ACTIVE(K3) ? 1 : 0;

        btn_now  = 0;
        btn_now |= (k1_on ? BTN_K1 : 0);
        btn_now |= (k2_on ? BTN_K2 : 0);
        btn_now |= (k3_on ? BTN_K3 : 0);
        btn_now |= (SW_ACTIVE(K4) ? BTN_K4 : 0);
        btn_now |= (SW_ACTIVE(K5) ? BTN_K5 : 0);

        /* -------------------------------------------------- 急停（锁存，仅独立形态）
         * 按下即置位，之后必须等方向柄**回到中立位**才解除；急停期间输出按中立走、
         * 三只方向灯全灭。USB 形态没有急停设计，estop 恒为 0。
         *
         * 这里刻意**不去改 dir** —— dir 始终反映方向柄的真实位置。若把 dir 强制归零，
         * "dir == 0" 这个解除条件就会立刻自满足，锁存等于没做。
         */
        if (!USB_HOST) {
            if (k1_on)                  estop = 1;      /* 按下立刻锁存 */
            else if (estop && dir == 0) estop = 0;      /* 松手且手柄已回中立才解除 */
        }
        dir_out = estop ? 0 : dir;                      /* 真正拿去驱动输出的方向 */

        /* ------------------------------------------- 每轮都算的读数（屏用）
         * 即使 USB 形态不驱动输出级，这几个量也照算 —— 本地屏显示的是"按当前开关
         * 拨法会整定成多少"。
         */
        duty = ADC_TO_DUTY(pwr);
        vout = setpointMv(k2_on, k3_on, v_off, pwr);

        /* -------------------------------------------------- 面板指示灯 */
        ledPanel(k2_on, dir, estop);

        /* -------------------------------------------------- 刷屏（限速 5Hz）
         * ms_tick 只在下面两个延时里累加，所以"一直在忙着上报"时刷屏自然让路，
         * 松手后立刻追上。
         */
        if ((uint16_t) (ms_tick - oled_last) >= OLED_REFRESH_MS) {
            oled_last = ms_tick;
            if (USB_HOST) {
                /* USB 形态：输出级没在驱动，改显示输入状态（值就是上报的那两个轴） */
                oledShowUsb((int16_t) (pwr - ADC_MID),
                            (int16_t) (v_off - ADC_MID),
                            dir, btn_now, isen);
            } else {
                oledShowLocal(vout, pwm_dhz[pwm_freq_idx],
                              (uint8_t) ((uint16_t) duty * 100u / PWM_MAX_DUTY),
                              v_off, isen, dir_out, estop);
            }
        }

        /* ------------------------------------- 出口 A：接在 PC 上 → 只上报 */
        if (USB_HOST) {
            /* A1 功率盘 -> X，A3 右线性手柄 -> Y。
               两路都已是相对中点的 ±1023 偏差，而描述符里的逻辑范围就是
               JOY_AXIS_MIN..JOY_AXIS_MAX（= ADC 的 11 位有效分辨率），所以**原样填
               进轴**即可，不做缩放；钳位直接用描述符那对边界，免得两边写岔。
               注意减法是**无符号**相减、再按补码解释成有符号：值 = 0 得 -1023、
               值 = 1023 得 0。改成先转有符号再减反而不等价，别动。 */
            int16_t d_x = (int16_t) (pwr - ADC_MID);
            d_x = d_x < JOY_AXIS_MIN ? JOY_AXIS_MIN : d_x;
            d_x = d_x > JOY_AXIS_MAX ? JOY_AXIS_MAX : d_x;

            int16_t d_y = (int16_t) (v_off - ADC_MID);
            d_y = d_y < JOY_AXIS_MIN ? JOY_AXIS_MIN : d_y;
            d_y = d_y > JOY_AXIS_MAX ? JOY_AXIS_MAX : d_y;

            g_joy.buttons = (uint16_t) btn_now;
            g_joy.hat     = (dir == 1)  ? JOY_HAT_UP
                          : (dir == -1) ? JOY_HAT_DOWN
                                        : JOY_HAT_CENTER;
            g_joy.x       = d_x;
            g_joy.y       = d_y;
            g_joy.z       = JOY_AXIS_CENTER;    /* 没接的通道固定填中点，否则主轴 */
            g_joy.rz      = JOY_AXIS_CENTER;    /* 会看到悬空引脚导致的"自己乱飘" */

            if (joySendIfChanged()) {
                /* 内容变了并且已经发出：立刻进下一轮，尽快跟上操作变化 */
            } else {
                /* 内容没变 —— 拿这轮空档通过 vendor 接口发一行调试文本，
                   顺便把节奏放慢到 DBG_REFRESH_MS，别把 CPU 全耗在轮询上。
                   格式：D 方向 / A AIN_2 原值 / P A1 / V A3（P、V 是 >>3 的粗值）
                   ★ A 单独列出来是**故意的**：方向判别完全看 AIN_2 落在那三段里的
                     哪一段（见 oledShowLocal 的说明），只有看到原值才能判断是解码
                     错、还是开关分压正好压在 700/1400 的分档点上。 */
                sprintf(buff, "D%dA%dP%dV%dB%02X\n",
                        dir, ad_val, pwr >> 3, v_off >> 3, btn_now);
                joyPrint(buff);
                delay(DBG_REFRESH_MS);
                ms_tick += DBG_REFRESH_MS;
            }
        }
        /* -------------------------- 出口 B：独立控制器 → 自己驱动输出级 */
        else {
            /* 输出级形态由 K2 定（PWM 与电机共用 P4.3/P4.5，只在变化时重配） */
            setStage(k2_on);

            /* K4/K5 调 PWM 频率（自复位，取边沿，按一次一档） */
            if (!k4_held && SW_ACTIVE(K4))      { k4_held = 1; freqStep(0); }
            else if (k4_held && !SW_ACTIVE(K4)) { k4_held = 0; }
            if (!k5_held && SW_ACTIVE(K5))      { k5_held = 1; freqStep(1); }
            else if (k5_held && !SW_ACTIVE(K5)) { k5_held = 0; }

            /* 用 dir_out 而不是 dir：急停期间这里必须是中立 */
            if (k2_on) pwmByDir(dir_out, duty);
            else       motorByDir(dir_out);

            voltSet(vout);

            delay(IDLE_POLL_MS);
            ms_tick += IDLE_POLL_MS;
        }
    }
}

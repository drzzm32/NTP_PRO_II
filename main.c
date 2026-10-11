/* ============================================================================
 * NTP PRO II —— 应用层（CH559）
 *
 * 这个板子有**两种形态**，在**上电时判定一次**，之后整个供电周期内不再改变：
 *
 *   USB 在手  → 它是一只标准手柄：把挡位、按钮、两路模拟量打包成 HID 报表上报，
 *               接口契约见 joy.h。**此时不驱动输出级** —— 主机在遥控，板子只负责
 *               把操作意图报上去。
 *   USB 不在  → 它是一台独立控制器：挡位直接驱动输出级（电机 H 桥 或 PWM），
 *               板子自己干活，不依赖主机。
 *
 * 为什么可以"判定一次"：本板要么接 PC（USB 总线上有主机，会发 SET_CONFIGURATION），
 * 要么接 USB PD 电源（**供电协商由另一颗芯片负责，CH559 不是这条路上的主机**）。
 * 两种形态在一次上电里不可能互变，要换形态只能断电重上。所以主循环里不需要反复问
 * joyReady()，形态在 waitUsbHost() 里定下来之后就是常量。
 *
 * 判定手段是"等 SET_CONFIGURATION"，而不是"看总线上有没有电平活动"：PD / QC 一类
 * 充电器会在 D+/D- 上做 BC1.2 / QC 传统握手（拉固定电平、甚至把 D+/D- 短起来），
 * 只看电平会把充电器误判成主机。SET_CONFIGURATION 只有真主机才发得出来。
 *
 * 输出级类型同样在**上电瞬间**由 KEY_1 选：松开 = 电机 H 桥（默认），按住 = 双路 PWM。
 *
 * 主循环的控制流（见 main() 末尾，四段顺序固定）：
 *   采集 → 挡位杆解码 → 方向指示灯 → 按形态二选一出口
 * ========================================================================== */

#include "ch559.h"

#include <stdio.h>

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

/* 这两项都在上电时定下来、之后只读。放文件作用域一是省栈，二是后面接 OLED 显示
   时可以直接读。 */
__bit PWM_MODE = false;     /* 输出级类型：true = 双路 PWM，false = 电机 H 桥 */
__bit USB_HOST = false;     /* 形态：true = 接在 PC 上（手柄），false = 独立控制器 */

/* USB 层只读这个结构，业务层负责写。定义放在应用侧，语义更清楚：
 * "这一帧要报什么"是应用的状态，不是 USB 的状态。 */
joy_state_t g_joy;


/* ============================================================================
 * 量纲与常量
 *
 * ADC 名义 12 位，但本项目只取低 **11** 位（& ADC_MASK → 0..2047），中点是 1023。
 * 也就是说一次采样的有效对称区间就是 -1023..+1023 —— 手柄描述符里的逻辑范围正是
 * 照这个写的（见 joy.h 的"模拟轴"）。下面所有换算都建立在这个 11 位量程上。
 * ========================================================================== */

#define ADC_MASK        0x07FFu     /* 有效位掩码：11 位 */
#define ADC_FULL        2047u       /* 11 位满量程计数（= ADC_MASK），用于 mV 换算 */
#define ADC_MID         1023u       /* 上述区间的中点 */
#define ADC_REF_MV      3300u       /* ADC 参考电压 3.3V */

/* 挡位杆（AIN_1）：三位置开关，靠 ADC 分档读出 -1 / 0 / +1。
   0..699 → 0 档，700..1399 → 1 档，1400..2047 → 2 档；再减 1 得到 -1/0/+1。 */
#define LEVER_STEP      700u

/* 电流采样（ISEN）换算：零电流时输出 1650 mV，灵敏度 132 mV/A，硬件分压 4 倍。
   ⚠️ 这三个常量**不能带 u 后缀**：一带上整个表达式就从有符号除法变成无符号除法，
   负电流（充电）算出来的值就变了。
   ⚠️ 末尾那个"除以 4"的方向按原理图核一下：如果取样放大器是 4 倍**增益**，
   这里应该是乘 4 而不是除 4。 */
#define MV_PER_V        1000
#define ISEN_ZERO_MV    1650
#define ISEN_MV_PER_A   132
#define ISEN_DIV        4

/* 轨道电压换算：AIN_2(v_off) 与 AIN_3(pwr) 都是 0..2047，每计数算 6 mV，
   满量程 2047 × 6 ≈ 12.28 V。voltSet() 收毫伏，内部再映射到 DAC5571 的 0..255。 */
#define TRACK_MV_PER_COUNT  6u

/* 0..2047 的 ADC 读数 → 0..255 的占空比（pwmSet() 的量程）。 */
#define PWM_MAX_DUTY    255u
#define ADC_TO_DUTY(v)  ((uint8_t) ((v) >> 3))

/* 手柄按键位。**原样**搬进报文的按键字段（byte0 的低 8 位），所以主机看到的前 8
   个按钮和改造前完全一致。bit4/bit5 是"方向键"位，另外 dir 还会报进 hat ——
   旧描述符没有 hat，方向只能靠这两位表达。 */
#define BTN_KEY1        0x01u
#define BTN_KEY2        0x02u
#define BTN_KEY3        0x04u
#define BTN_DIR_FWD     0x10u
#define BTN_DIR_REV     0x20u

/* 节奏 */
#define DBG_REFRESH_MS  100u        /* 无输入变化时，刷新调试文本的间隔 */
#define IDLE_POLL_MS    20u         /* 独立控制器模式下的轮询间隔 */

/* 形态判定的等待窗口：初始化时最多等这么久，等主机发 SET_CONFIGURATION。
   ⚠️ 这个值有两个含义，调的时候两边都要想：
     * 接 USB PD 电源时，它就是"上电后要多等多久才开始驱动输出级"；
     * 接 PC 时，它是"主机慢到多久就会被误判成独立控制器"的界线。
   2000ms 对 PC 一侧非常宽松（Windows/Linux 从接入到 SET_CONFIGURATION 通常
   远小于 500ms），板子上实测后可以调。 */
#define USB_MODE_TIMEOUT_MS 2000u
#define USB_MODE_POLL_MS    50u


/* ---------------------------------------------------------------- 形态判定
 *
 * 上电时调一次，返回是否接在 PC 上。见文件头"为什么可以判定一次"。
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


/* ---------------------------------------------------------------- 模拟量换算
 *
 * 这两个是"给显示/调试用的物理量"，主循环每轮都会算好放进 vout / isen。
 * OLED 目前只显示固定标题，接上电压/电流显示时直接读这两个变量即可。
 */

/* ISEN 通道的 ADC 读数 → 电流（mA）。正 = 放电，负 = 充电。
   ⚠️ 中间量**必须走 32 位**：ad_val 最大 2047，2047 × 3300 ≈ 6.75e6，16 位放不下。
   （原来的写法就是 16 位相乘，ad_val > 19 即溢出，算出来的电流全是垃圾。） */
static int16_t currentMa(uint16_t ad_val) {
    int32_t mv = ((int32_t) ad_val * ADC_REF_MV) / ADC_FULL;    /* 先换算成 mV */
    return (int16_t) (((mv - ISEN_ZERO_MV) * MV_PER_V) / ISEN_MV_PER_A / ISEN_DIV);
}

/* 整定输出电压（mV）。两种形态的公式**不同**：PWM 形态只看 v_off，
   电机形态把两路相加 —— 这是原有行为，不是笔误。 */
static uint16_t trackMv(uint16_t v_off, uint16_t pwr) {
    if (PWM_MODE)
        return (uint16_t) (v_off * TRACK_MV_PER_COUNT);
    return (uint16_t) (pwr * TRACK_MV_PER_COUNT + v_off);
}


/* ---------------------------------------------------------------- 输出级动作 */

/* 按挡位拨电机 H 桥（KEY_1 松开的默认形态）。
   中立 = 两端都断开（惯性滑行），不刹车。 */
static void motorByDir(int8_t dir) {
    switch (dir) {
        case -1:  MOT_F = 0; MOT_R = 1; break;
        case  0:  MOT_F = 0; MOT_R = 0; break;
        case  1:  MOT_F = 1; MOT_R = 0; break;
        default:  MOT_F = 1; MOT_R = 1; break;   /* 挡位被钳在 ±1，正常到不了这里 */
    }
}

/* 按挡位拨两路 PWM（KEY_1 按住的上电形态）。duty 是 **0..255**，调用方负责换算。 */
static void pwmByDir(int8_t dir, uint8_t duty) {
    switch (dir) {
        case -1:  pwmSet(1, 0);           pwmSet(2, duty); break;
        case  0:  pwmSet(1, 0);           pwmSet(2, 0);    break;
        case  1:  pwmSet(1, duty);        pwmSet(2, 0);    break;
        default:  pwmSet(1, PWM_MAX_DUTY); pwmSet(2, PWM_MAX_DUTY); break;
    }
}

/* 方向指示灯：PRT_0 = 前进，PRT_1 = 中立，PRT_2 = 后退（高电平点亮）。
   挡位被钳在 ±1，所以 default 正常到不了 —— 留着当"异常时全亮"的兜底。 */
static void ledByDir(int8_t dir) {
    switch (dir) {
        case -1:  PRT_0 = 0; PRT_1 = 0; PRT_2 = 1; break;
        case  0:  PRT_0 = 0; PRT_1 = 1; PRT_2 = 0; break;
        case  1:  PRT_0 = 1; PRT_1 = 0; PRT_2 = 0; break;
        default:  PRT_0 = 1; PRT_1 = 1; PRT_2 = 1; break;
    }
}


void main() {
    /* ---------------------------------------------------------------- 上电初始化 */

    LED = 1;                    /* 先灭灯、松开电机，避免上电瞬间乱动 */
    MOT_F = 0; MOT_R = 0;

    sysPinConfig();
    sysClockConfig();
    delay(10);

    oledInit();
    oledLogo();                 /* 开机 logo，停 1 秒给人看 */
    delay(1000);

    /* 输出级类型在这里定下来：按住 KEY_1 上电 → PWM 形态 */
    PWM_MODE = (KEY_1 == 0);
    if (PWM_MODE)
        pwmInit();

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

    usbDevInit();
    EA = 1;                     /* 中断全开：USB 和 PWM 的中断从这里开始服务 */

    oledClear(0x0);
    oledStringC(128, 32, 1, 0x0F, "NTP PRO II");

    /* 形态判定：接在 PC 上还是独立控制器。这一次判定贯穿整个供电周期。 */
    USB_HOST = waitUsbHost();

    /* 让"上一帧"缓存失效，这样主机 SET_CONFIGURATION 之后的第一帧必定发出去
       （原来这里是 releaseAllKey() + pushKeyData()，效果相同）。独立形态下这两句
       是无害空操作。 */
    joyInvalidate();
    joyRequestHostData();

    /* ------------------------------------------------- 循环状态（跨轮保留） */
    int8_t  dir     = 0;        /* 挡位：-1 后退 / 0 中立 / 1 前进。被钳在 ±1 */
    int8_t  dir_raw = 0;        /* 挡位杆这一轮的**原始**读数：-1 / 0 / +1 */
    uint8_t up_held = 0;        /* 边沿检测记忆：上一轮挡位杆是否停在 +1 */
    uint8_t dn_held = 0;        /* 同上，-1 */
    uint8_t k1_held = 0;        /* 同上，KEY_1；独立形态下用它把挡位拉回中立 */

    /* ------------------------------------------------- 每轮刷新的采样值 */
    uint16_t ad_val = 0;        /* ADC 原始读数（0..2047），多路复用 */
    uint16_t v_off  = 0;        /* AIN_2：电压整定 */
    uint16_t pwr    = 0;        /* AIN_3：功率整定 */
    uint16_t vout   = 0;        /* 整定输出电压（mV）。每轮都算，供 output/显示 用 */
    int16_t  isen   = 0;        /* 实测电流（mA）。每轮都算，供显示/调试用 */

    while (1) {
        /* ------------------------------------------------------------ 采集
         * adcGet() 是**单通道复用**的：读一次换一路，所以下面四行的顺序就是扫描
         * 顺序，不要合并或提前。
         */
        isen    = currentMa(adcGet(ISEN) & ADC_MASK);       /* 第 1 路：电流 */
        ad_val  = adcGet(AIN_1) & ADC_MASK;                 /* 第 2 路：挡位杆 */
        v_off   = adcGet(AIN_2) & ADC_MASK;                 /* 第 3 路：电压整定 */
        pwr     = adcGet(AIN_3) & ADC_MASK;                 /* 第 4 路：功率整定 */

        dir_raw = (int8_t) (ad_val / LEVER_STEP) - 1;       /* 分档 → -1 / 0 / +1 */
        vout    = trackMv(v_off, pwr);

        /* -------------------------------------------------- 挡位杆解码
         * AIN_1 接的是一个**自复位**三位置拨杆，不是绝对挡位选择器：
         * 每拨到 +1 一次，挡位 +1；每拨到 -1 一次，挡位 -1；都钳在 ±1。
         * 所以"前进 → 中立"要往回拨一次，"中立 → 前进"要往前拨一次。
         * 用 up_held/dn_held 做边沿检测，否则拨杆停在一侧会持续累加。
         */
        if (!up_held && dir_raw == 1) {
            up_held = 1;
            dir += 1;
            dir = dir > 1 ? 1 : dir;
        } else if (up_held && dir_raw != 1) {
            up_held = 0;
        }

        if (!dn_held && dir_raw == -1) {
            dn_held = 1;
            dir -= 1;
            dir = dir < -1 ? -1 : dir;
        } else if (dn_held && dir_raw != -1) {
            dn_held = 0;
        }

        /* 独立形态下的"回中立"：手柄形态下 KEY_1 是普通按钮、由主机去解读，
           所以这里只在不接 PC 时把它当急停/回中位用。 */
        if (!USB_HOST) {
            if (!k1_held && KEY_1 == 0) {
                k1_held = 1;
                dir = 0;
            } else if (k1_held && KEY_1 != 0) {
                k1_held = 0;
            }
        }

        /* -------------------------------------------------- 方向指示灯 */
        ledByDir(dir);

        /* ------------------------------------- 出口 A：接在 PC 上 → 上报手柄 */
        if (USB_HOST) {
            uint8_t btn = 0;
            btn |= (KEY_1 == 0 ? BTN_KEY1 : 0);
            btn |= (KEY_2 == 0 ? BTN_KEY2 : 0);
            btn |= (KEY_3 == 0 ? BTN_KEY3 : 0);
            btn |= (dir == 1  ? BTN_DIR_FWD : 0);
            btn |= (dir == -1 ? BTN_DIR_REV : 0);

            /* 两路模拟量都已经是相对中点的 ±1023 偏差，而描述符里的逻辑范围就是
               JOY_AXIS_MIN..JOY_AXIS_MAX（= ADC 的 11 位有效分辨率），所以**原样填进
               轴**即可，不做任何缩放；钳位也直接用描述符那对边界，免得两边写岔。
               注意减法是**无符号**相减、再按补码解释成有符号：v_off = 0 得 -1023、
               v_off = 1023 得 0。改成先转有符号再减反而不等价，别动。 */
            int16_t d_l = (int16_t) (v_off - ADC_MID);
            d_l = d_l < JOY_AXIS_MIN ? JOY_AXIS_MIN : d_l;
            d_l = d_l > JOY_AXIS_MAX ? JOY_AXIS_MAX : d_l;

            int16_t d_r = (int16_t) (pwr - ADC_MID);
            d_r = d_r < JOY_AXIS_MIN ? JOY_AXIS_MIN : d_r;
            d_r = d_r > JOY_AXIS_MAX ? JOY_AXIS_MAX : d_r;

            g_joy.buttons = (uint16_t) btn;
            g_joy.hat     = (dir == 1)  ? JOY_HAT_UP
                          : (dir == -1) ? JOY_HAT_DOWN
                                        : JOY_HAT_CENTER;
            g_joy.x       = d_l;
            g_joy.y       = d_r;
            g_joy.z       = JOY_AXIS_CENTER;    /* 没接的通道固定填中点，否则主轴 */
            g_joy.rz      = JOY_AXIS_CENTER;    /* 会看到悬空引脚导致的"自己乱飘" */

            if (joySendIfChanged()) {
                /* 内容变了并且已经发出：立刻进下一轮，尽快跟上操作变化 */
            } else {
                /* 内容没变 —— 拿这轮空档通过 vendor 接口发一行调试文本，
                   顺便把节奏放慢到 DBG_REFRESH_MS，别把 CPU 全耗在轮询上。 */
                sprintf(buff, "D%dP%dV%dB%02X\n", dir, pwr >> 3, v_off >> 3, btn);
                /* 想改成显示电压/电流就换下面这行（isen / vout 每轮都已经算好了）：
                   sprintf(buff, "Vo:%2d.%1dV, Is:%4dmA\n",
                           vout / 1000, (vout % 1000) / 100, isen); */
                joyPrint(buff);
                delay(DBG_REFRESH_MS);
            }
        }
        /* ------------------------ 出口 B：独立控制器 → 自己驱动输出级 */
        else {
            if (PWM_MODE) {
                pwmByDir(dir, ADC_TO_DUTY(pwr));
            } else {
                motorByDir(dir);
            }

            voltSet(vout);

            delay(IDLE_POLL_MS);
        }
    }
}

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
 * OLED（256x64 / 4 位灰度）两种形态各一屏，见 uiShowLocal() / uiShowUsb()。
 * 界面是扁平化 + 灰度分层，右侧一列方向箭头和那三只物理 LED 等高对应，
 * 顶部灰底状态带对应右上角的电源/PWM 两只 LED —— 画法和理由写在下面的
 * "OLED 界面"那段注释里，这里不重复。
 *   ⚠️ 字库只有 ASCII（font.c 的索引是 c - ' '），所以标签只能用英文缩写。
 *   ⚠️ 方向**单独占一列**显示。这一路要经过机械开关 + ADC 分档，是整屏里唯一依赖
 *      模拟量正确性的一项 —— 实测就靠它抓到过 A2 电位器焊错。细节见 uiShowLocal()。
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
#include "fontzh.h"

/* 中断向量里的名字必须和 usb.c / sys.c 里的定义完全一致。
   ⚠️ 这里**故意不写 __using**：8051 的每个寄存器组占 8 字节直接寻址区，而本工程
   那块只有 128 字节、已经被挤满（加中文字库时链接就报 OSEG 分配不到）。
   SDCC 对没有 __using 的中断会自己 PUSH/POP 用到的寄存器，一样安全，只是慢几个
   周期 —— USB 中断 1kHz、PWM 中断只清标志，这点开销无所谓。 */
void __usb_interrupt()  __interrupt (INT_NO_USB);
void __pwm1_interrupt() __interrupt (INT_NO_PWM1);

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
   刷屏只要 5Hz，这个精度足够；间隔用无符号减法判，回绕不会出错。
   显式 __xdata：理由同上（给只有 128 字节的直接寻址区让位）。 */
#define OLED_REFRESH_MS 200u
__xdata uint16_t ms_tick   = 0;
__xdata uint16_t oled_last = 0;


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


/* ============================================================================
 * OLED 界面：扁平化 + 灰度分层
 *
 * 界面怎么排，是面板的物理排布决定的 —— 屏幕右侧从上到下挂的是三只方向 LED
 * （正向 / 中位 / 反向），屏幕右上角是电源 LED 与 PWM LED：
 *
 *        ┌──────────────── 屏幕 ────────────────┐     ▲ 正向 LED
 *        │                                      │     ● 中位 LED
 *        └──────────────────────────────────────┘     ▼ 反向 LED
 *                                  右上角：电源 LED / PWM LED
 *
 * 所以三只方向箭头分别落在 y=16 / 32 / 48 三行，和右边那三只 LED **等高对应**，
 * 眼睛不用来回找；顶部再留一条灰底状态带，对应右上角那两只 LED。
 *
 * 灰度怎么用（这是 4 位灰度屏最值钱的地方，比画边框划算得多）：
 *   1. 状态带整条铺灰底 —— 用**背景灰度**分层，不画边框，这才是扁平；
 *   2. 标签用暗灰(fg=8)、数值用纯白(fg=15)，同一行里自然分出主次；
 *   3. 条形图 = 暗灰轨道(gray 3) + 纯白填充(gray 15)；
 *   4. 三只方向箭头**始终都在**，当前档纯白、其余只留暗灰(fg=5) —— 既看得出
 *      "一共三档"，又不必画任何框线。急停时三只全暗，和"三只物理 LED 全灭"一致。
 *
 * 布局（256x64；主行用 8x16 字库，两行次要信息用 8x8 挤在底部）：
 *
 *   独立形态                                          USB 形态
 *   y= 0  灰带 NTP PRO II   STOP LOCAL PWM            灰带 NTP PRO II    USB  PWM
 *   y=16  Vset ▐██████████░░▌ 12.28V               ▲  X   ▐░░░░██│██░░░░▌  1023
 *   y=32  Duty ▐███████████░▌  100%                ●  Y   ▐███░░░│░░░░░░▌ -1023
 *   y=48  Isen  1250mA  Fpwm 109.4kHz              ▼  Btn K1--K3----
 *   y=56  Voff  2047mV
 *
 * 两种形态骨架相同（状态带 + 两条条形图 + 两行小字 + 右侧箭头列），只是内容不同：
 * 独立形态的条从 0 起（整定电压 / 占空比）；USB 形态的条**以中点为 0**，往左是负、
 * 往右是正（两个轴）—— 这样一眼就能看出杆在中位还是偏了。
 *
 * 做法与约束：
 *   * 字库只有 ASCII（font.c 的索引是 c - ' '），标签只能用英文缩写；
 *   * 文本一律**固定宽度**格式化，否则数值位数变少时覆盖写会留下上一帧的尾巴；
 *   * 状态带 / 方向 / 条形填充各自有缓存，**值没变就不发 SPI**；
 *   * 方向和按钮两处手工拼接，不用 %s（省得把 printf 的字符串支持也链进来）。
 * ========================================================================== */

/* ---- 调色板：color[3:0] = 前景(亮)，color[7:4] = 背景 ---- */
#define UI_BAND_BG      6           /* 状态带底色：中灰，和黑底拉开层次 */
#define UI_TXT          0x0F        /* 黑底白字：数值与主要信息 */
#define UI_LABEL        0x08        /* 黑底暗灰字：标签，不跟数值抢注意力 */
#define UI_TRACK        3           /* 条形图轨道：很暗的灰 */
#define UI_FILL         15          /* 条形图填充：纯白 */
#define UI_BAND_TXT     0x6F        /* 灰底白字（bg 6 + fg 15） */
#define UI_BAND_DIM     0x67        /* 灰底暗灰字：表示"这一项是关的" */
#define UI_DIR_HOT      0x0F        /* 当前档的方向箭头：黑底纯白 */
#define UI_DIR_DIM      0x05        /* 其余档：黑底暗灰，只留个位置提示 */

/* ---- 布局 ---- */
#define UI_BAND_Y       0
#define UI_BAND_H       16
#define UI_ROW1_Y       16          /* 第一条：Vset / X */
#define UI_ROW2_Y       32          /* 第二条：Duty / Y */
#define UI_INFO1_Y      48          /* 8x8 小字第一行 */
#define UI_INFO2_Y      56          /* 8x8 小字第二行 */
#define UI_BAR_X        40
#define UI_BAR_W        128
#define UI_BAR_H        10
#define UI_BAR_DY       4           /* 16 行里让 10 高的条垂直居中 */
#define UI_VAL_X        172         /* 主行数值的 x */
#define UI_ARROW_X      240         /* 方向箭头列：贴屏幕右边，对齐右侧三只 LED */
#define UI_BAND_TITLE_X 8
#define UI_BAND_STOP_X  96
#define UI_BAND_MODE_X  176
#define UI_BAND_PWM_X   224

/* 条形图满量程。Vset 取整定的真实上限（pwr*6+v_off 最大 = 2047*6+2047 = 14329mV），
   这样整条范围内单调；Duty 用 100%；轴用 ±1023。 */
#define UI_VSET_FULL_MV 14329u
#define UI_AXIS_FULL    1023

/* ---- 方向箭头（16x16 一位点阵）。字节布局与 oledBinary 一致：
        byte[f] 的 bit[row%8]，f = 16*(row/8) + col。生成时往返自检过。 ---- */
const static uint8_t ui_dir_up[32] = {
    0x00, 0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC,
    0xFC, 0xF8, 0xF0, 0xE0, 0xC0, 0x80, 0x00, 0x00,
    0x3E, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
    0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3E,
};
const static uint8_t ui_dir_mid[32] = {
    0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0,
    0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0, 0xE0,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07,
};
const static uint8_t ui_dir_dn[32] = {
    0x3C, 0x7C, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC,
    0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0xFC, 0x7C, 0x3C,
    0x00, 0x00, 0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F,
    0x1F, 0x0F, 0x07, 0x03, 0x01, 0x00, 0x00, 0x00,
};

/* ---- 动态元素的缓存：值没变就不发 SPI ----
   ⚠️ 这两个缓冲按**字节**算：一个中文占 3 个 UTF-8 字节却只占 1 格，所以要比
   看起来的宽度宽松得多（下面最长的一行 "电流  1250mA  频率 109.4kHz" 是 31 字节）。
   ⚠️ 全部显式加 __xdata：SDCC 的 --model-large **只管指针宽度**，变量的默认存储
   仍是 __data —— 而直接寻址区一共只有 128 字节，本工程已经被 oled/main/sys 几个
   模块占满（加中文字库时就是卡在这里：链接报 "Couldn't get N bytes ... for
   area OSEG"）。UI 这些量一轮只碰几次，放 xdata 慢一点无所谓。 */
static __xdata char     ui_line[4][40]; /* 这一帧的 4 段文本（UTF-8） */
static __xdata char     ui_prev[4][40]; /* 上一次真正画上去的 */
static __xdata uint16_t ui_px[2];       /* 两条条的填充（0 起的宽度，或中点起的带符号像素） */
static __xdata int8_t   ui_dir_c  = -2; /* 已画的方向 */
static __xdata uint8_t  ui_estop_c;     /* 已画的急停状态 */
static __xdata uint8_t  ui_band_c;      /* 已画的状态带（模式/PWM/急停 的位组合） */
static __xdata uint8_t  ui_dirty  = 1;  /* 1 = 缓存全部无效（刚上电/换屏），本帧全画 */

/* 把 ui_line[i] 推上屏（内容没变就跳过）。
   统一走 oledStringZh()：纯 ASCII 时它逐个调 oledChar，效果和 oledString() 一样，
   有中文时自动查子集字库 —— 这样不用维护两套调用。 */
static void uiFlush(uint8_t i, uint16_t x, uint16_t y, uint8_t fid, uint8_t color) {
    if (strcmp(ui_line[i], ui_prev[i]) != 0) {
        oledStringZh(x, y, fid, color, ui_line[i]);
        strcpy(ui_prev[i], ui_line[i]);
    }
}

/* 顶部状态带：整条灰底 + 左标题 + 右侧三格（急停 / 模式 / PWM）。
   整条 256x16 是 1KB SPI，只在状态变化时重画，不值得每轮都发。 */
static void uiBand(uint8_t usb, uint8_t pwm_on, uint8_t estop) {
    uint8_t c = (uint8_t) ((usb ? 1u : 0u) | (pwm_on ? 2u : 0u) | (estop ? 4u : 0u));
    if (!ui_dirty && c == ui_band_c) return;
    ui_band_c = c;

    oledRect(0, UI_BAND_Y, OLED_WIDTH, UI_BAND_H, UI_BAND_BG);
    oledString(UI_BAND_TITLE_X, UI_BAND_Y, 1, UI_BAND_TXT, "NTP PRO II");
    /* 急停用中文更醒目；状态带每次变化都是整条重画，所以不用手动擦掉旧字 */
    if (estop) oledStringZh(UI_BAND_STOP_X, UI_BAND_Y, 1, UI_BAND_TXT, "急停");
    oledStringZh(UI_BAND_MODE_X,  UI_BAND_Y, 1, UI_BAND_TXT, usb ? "USB" : "本机");
    oledString(UI_BAND_PWM_X,   UI_BAND_Y, 1, pwm_on ? UI_BAND_TXT : UI_BAND_DIM, "PWM");
}

/* 条形图（从 0 起）：先铺轨道再铺填充，px 是填充宽度。
   ⚠️ px 向下取整到 4 的倍数：SSD1322 的列地址横向粒度是 **4 个像素**（见 oledTransfer
   里那段说明），窗口会向上对齐，所以取整之后"数值 → 条长"才是确定的，
   不会多冒 1~3 个像素。代价是条长只有 128/4 = 32 档，做指示足够。 */
static void uiBar(uint8_t idx, uint16_t y, uint16_t px) {
    px &= (uint16_t) ~3u;                       /* 向下取整到 4 的倍数 */

    if (!ui_dirty && px == ui_px[idx]) return;
    ui_px[idx] = px;

    oledRect(UI_BAR_X, y + UI_BAR_DY, UI_BAR_W, UI_BAR_H, UI_TRACK);
    if (px) oledRect(UI_BAR_X, y + UI_BAR_DY, px, UI_BAR_H, UI_FILL);
}

/* 条形图（以中点为 0）：USB 形态看两个轴用 —— 往左是负、往右是正。
   中点 UI_BAR_X + UI_BAR_W/2 = 104 本身就是 4 的倍数，所以长度取整到 4 之后
   左端 (104 - n*4) 与右端都还落在 4 的倍数上，和列地址对齐一致。 */
static void uiBarMid(uint8_t idx, uint16_t y, int16_t v) {
    uint16_t half = UI_BAR_W / 2;
    int16_t  px   = (int16_t) ((int32_t) v * (int32_t) half / UI_AXIS_FULL);

    px = (int16_t) (px / 4 * 4);                /* 同 uiBar：取整到 4 的倍数 */

    if (!ui_dirty && (uint16_t) px == ui_px[idx]) return;
    ui_px[idx] = (uint16_t) px;

    oledRect(UI_BAR_X, y + UI_BAR_DY, UI_BAR_W, UI_BAR_H, UI_TRACK);
    if (px > 0)
        oledRect((uint16_t) (UI_BAR_X + half), y + UI_BAR_DY, (uint16_t) px, UI_BAR_H, UI_FILL);
    else if (px < 0)
        oledRect((uint16_t) (UI_BAR_X + half + px), y + UI_BAR_DY,
                 (uint16_t) (-px), UI_BAR_H, UI_FILL);
}

/* 方向箭头列。三只始终都画：当前档纯白、其余暗灰；急停时三只全暗。
   位置 y=16/32/48 与屏幕右边那三只物理 LED 等高对应。 */
static void uiDir(int8_t dir, uint8_t estop) {
    if (!ui_dirty && dir == ui_dir_c && estop == ui_estop_c) return;
    ui_dir_c   = dir;
    ui_estop_c = estop;

    oledBinary(UI_ARROW_X, UI_ROW1_Y, 16, 16,
               (!estop && dir ==  1) ? UI_DIR_HOT : UI_DIR_DIM, (uint8_t*) ui_dir_up);
    oledBinary(UI_ARROW_X, UI_ROW2_Y, 16, 16,
               (!estop && dir ==  0) ? UI_DIR_HOT : UI_DIR_DIM, (uint8_t*) ui_dir_mid);
    oledBinary(UI_ARROW_X, UI_INFO1_Y, 16, 16,
               (!estop && dir == -1) ? UI_DIR_HOT : UI_DIR_DIM, (uint8_t*) ui_dir_dn);
}

/* 静态框架：两条主行的标签。开机画一次，之后不再动。
   （条形图的轨道不在里画 —— 它由 uiBar/uiBarMid 在第一次刷新时连同填充一起铺。） */
static void uiFrame(uint8_t usb) {
    /* 中文标签走 8x8 子集字库，oledStringZh 会把它在 16px 的行里垂直居中 */
    oledStringZh(0, UI_ROW1_Y, 1, UI_LABEL, usb ? "X轴"   : "电压");
    oledStringZh(0, UI_ROW2_Y, 1, UI_LABEL, usb ? "Y轴"   : "占空比");
}

/* 独立形态：整定电压 / 占空比 / 频率 / 偏置 / 电流 / 方向 */
static void uiShowLocal(uint8_t pwm_on, int8_t dir_out, uint8_t estop,
                        uint16_t vset_mv, uint8_t duty_pct, uint16_t dhz,
                        uint16_t voff_mv, int16_t isen_ma) {
    uiBand(0, pwm_on, estop);
    uiDir(dir_out, estop);
    uiBar(0, UI_ROW1_Y, (uint16_t) ((uint32_t) vset_mv * UI_BAR_W / UI_VSET_FULL_MV));
    uiBar(1, UI_ROW2_Y, (uint16_t) ((uint16_t) duty_pct * UI_BAR_W / 100u));

    sprintf(ui_line[0], "%3u.%02uV", vset_mv / 1000u, (vset_mv % 1000u) / 10u);
    sprintf(ui_line[1], "%4u%%", duty_pct);
    /* 8x8 小字行：中文和 8x8 的 ASCII 同高，正好混排（此处最长 31 字节） */
    sprintf(ui_line[2], "电流%6dmA  频率%4u.%1ukHz", isen_ma, dhz / 10u, dhz % 10u);
    sprintf(ui_line[3], "偏置%5umV", voff_mv);

    uiFlush(0, UI_VAL_X, UI_ROW1_Y, 1, UI_TXT);
    uiFlush(1, UI_VAL_X, UI_ROW2_Y, 1, UI_TXT);
    uiFlush(2, 0, UI_INFO1_Y, 0, UI_TXT);           /* 8x8 小字 */
    uiFlush(3, 0, UI_INFO2_Y, 0, UI_TXT);

    ui_dirty = 0;
}

/* USB 形态：两个轴（条形图以中点为 0）/ 按下的开关 / 电流 / 方向 */
static void uiShowUsb(uint8_t pwm_on, int8_t dir, int16_t ax, int16_t ay,
                      uint8_t btn, int16_t isen_ma) {
    uint8_t i, k;

    uiBand(1, pwm_on, 0);                   /* USB 形态没有急停 */
    uiDir(dir, 0);
    uiBarMid(0, UI_ROW1_Y, ax);
    uiBarMid(1, UI_ROW2_Y, ay);

    /* %6d 而不是 %5d：轴值能到 ±1023，负号会把 %5d 占满，和左侧标签黏在一起 */
    sprintf(ui_line[0], "%6d", ax);
    sprintf(ui_line[1], "%6d", ay);
    /* 按钮位图手工拼。⚠️ 用 strlen 取偏移而不是写死数字 —— 一个中文占 3 个 UTF-8
       字节却只占 1 格，写死字节下标改一次中文就错位。 */
    strcpy(ui_line[2], "按键 ");
    k = (uint8_t) strlen(ui_line[2]);
    for (i = 0; i < 5; i++) {
        uint8_t on = (btn & (1u << i)) ? 1 : 0;
        ui_line[2][k + i * 2]     = on ? 'K' : '-';
        ui_line[2][k + i * 2 + 1] = on ? (char) ('1' + i) : '-';
    }
    ui_line[2][k + 5 * 2] = '\0';
    sprintf(ui_line[3], "电流%6dmA", isen_ma);

    uiFlush(0, UI_VAL_X, UI_ROW1_Y, 1, UI_TXT);
    uiFlush(1, UI_VAL_X, UI_ROW2_Y, 1, UI_TXT);
    uiFlush(2, 0, UI_INFO1_Y, 0, UI_TXT);
    uiFlush(3, 0, UI_INFO2_Y, 0, UI_TXT);

    ui_dirty = 0;
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

    /* 形态判定期间屏上不至于全黑：先把状态带和标题铺上（此刻还不知道形态，
       所以不碰 ui_band_c 缓存 —— 等形态定下来后 uiBand() 会连状态一起重画）。 */
    oledClear(0x0);
    oledRect(0, UI_BAND_Y, OLED_WIDTH, UI_BAND_H, UI_BAND_BG);
    oledString(UI_BAND_TITLE_X, UI_BAND_Y, 1, UI_BAND_TXT, "NTP PRO II");

    /* 形态判定：接在 PC 上还是独立控制器。这一次判定贯穿整个供电周期。 */
    USB_HOST = waitUsbHost();

    /* 形态定了：画静态框架（两条主行的标签），动态部分交给后面 5Hz 的刷新 */
    uiFrame(USB_HOST);

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
                uiShowUsb(k2_on, dir,
                          (int16_t) (pwr - ADC_MID),
                          (int16_t) (v_off - ADC_MID),
                          btn_now, isen);
            } else {
                uiShowLocal(k2_on, dir_out, estop, vout,
                            (uint8_t) ((uint16_t) duty * 100u / PWM_MAX_DUTY),
                            pwm_dhz[pwm_freq_idx], v_off, isen);
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

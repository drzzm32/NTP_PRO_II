#include "sys.h"

#include "ch559.h"
#include "pin.h"

void sysPinConfig() {
    // FR RE LED
    P4_DIR |= 0xA8;

    // SCL SDA
    P2_DIR |= 0x03;
    P2_PU &= ~(0x03);

    // KEYs
    PORT_CFG &= ~(bP0_OC);
    P0_DIR = 0x00;
    P0_PU = 0x00;

    // PORTs
    PORT_CFG &= ~(bP3_OC);
    P3_DIR = 0xFF;
}

void sysClockConfig() {
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG &= ~MASK_SYS_CK_DIV;
    CLOCK_CFG |= (336000000 / FREQ_SYS);
    PLL_CFG = 0xFC; // fPll = 336MHz;
    SAFE_MOD = 0xFF;
#ifdef USE_EXT_OSC
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG |= bOSC_EN_XT;
    delay(10);
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG &= ~bOSC_EN_INT;
    SAFE_MOD = 0xFF;
#endif
}

void delay_us(uint16_t t) {
    while (t) {
		++ SAFE_MOD;
#ifdef	FREQ_SYS
#if		FREQ_SYS >= 14000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 16000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 18000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 20000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 22000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 24000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 26000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 28000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 30000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 32000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 34000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 36000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 38000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 40000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 42000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 44000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 46000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 48000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 50000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 52000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 54000000
		++ SAFE_MOD;
#endif
#if		FREQ_SYS >= 56000000
		++ SAFE_MOD;
#endif
#endif
		-- t;
	}
}

void delay(uint16_t n) {
	while (n) {
        delay_us(1000);
		-- n;
	}
}

size_t mc;

void mset(uint8_t* ptr, uint8_t val, size_t siz) {
    for (mc = 0; mc < siz; mc++)
        ptr[mc] = val;
}

void mmov(uint8_t* dst, uint8_t* src, size_t siz) {
    if (dst < src) {
        mc = 0;
        while (1) {
            dst[mc] = src[mc];
            mc += 1;
            if (mc == siz)
                break;
        }
    } else {
        mc = siz - 1;
        while (1) {
            dst[mc] = src[mc];
            if (mc == 0)
                break;
            mc -= 1;
        }
    }
}

uint32_t x; uint8_t x2; uint8_t dat;

void uart0Config(uint32_t baud) {
    SM0 = 0; SM1 = 1; SM2 = 0;
    TCLK = 0; RCLK = 0;
    PCON |= SMOD;

    T2MOD |= (bTMR_CLK | bT1_CLK);
    x = 10 * FREQ_SYS / baud / 16;
    if (x > 255) {
        T2MOD &= ~(bTMR_CLK);
        x = x / 4;
    }

    x2 = x % 10;
    x /= 10;
    if (x2 >= 5) x++;

    TMOD = TMOD & ~bT1_GATE & ~bT1_CT & ~MASK_T1_MOD | bT1_M1;
    TH1 = 0 - x;
    TR1 = 1;
    TI = 1;
    REN = 1;
    ES = 1;
}

#define UART_BUF_SIZE 32
volatile BOOL busy = 0;
static uint8_t recBuf0[UART_BUF_SIZE] = { 0 };
volatile uint8_t recPtr0 = 0;

BOOL block_recv = 0;
uint8_t* block_recv_ptr = NULL;
uint16_t block_recv_pos = 0;
uint16_t block_recv_siz = 0;

void __uart0_interrupt() __interrupt (INT_NO_UART0) __using (2) {
    if (TI) {
        TI = 0;
        busy = 0;
    }
    if (RI) {
        RI = 0;
        if (block_recv) {
            block_recv_ptr[block_recv_pos] = SBUF;
            block_recv_pos++;
            if (block_recv_pos >= block_recv_siz)
                block_recv = 0;
        } else if (recPtr0 < UART_BUF_SIZE) {
            recBuf0[recPtr0] = SBUF;
            recPtr0 += 1;
        }
    }
}

void uart0Send(uint8_t data) {
    while (busy);
    busy = 1;
    SBUF = data;
}

void uart0Write(char* s) {
    while (*s) {
        uart0Send(*s++);
    }
}

uint8_t uart0Get() {
    if (recPtr0 > 0) {
        dat = recBuf0[0];
        recPtr0 -= 1;
        if (recPtr0 > 0)
            mmov(recBuf0, recBuf0 + 1, recPtr0);
        mset(recBuf0 + recPtr0, 0, 1);
        return dat;
    } else
        return 0;
}

uint8_t uart0Gets(uint8_t* dat, uint8_t len) {
    if (recPtr0 >= len) {
        mmov(dat, recBuf0, len);
        recPtr0 -= len;
        if (recPtr0 > 0)
            mmov(recBuf0, recBuf0 + len, recPtr0);
        mset(recBuf0 + recPtr0, 0, len);
        return 1;
    } else
        return 0;
}

uint16_t uart0BlockRecv(uint8_t* ptr, uint16_t siz) {
    if (ptr != NULL) {
        block_recv_ptr = ptr;
        block_recv_pos = 0;
        block_recv_siz = siz;
        block_recv = 1;
    } else if (siz != 0) {
        return block_recv_pos;
    } else {
        block_recv = 0;
        block_recv_ptr = NULL;
        block_recv_pos = 0;
        block_recv_siz = 0;
    }
    return 0;
}

void adcInit() {
    P1_IE = 0x00; // P1x
    ADC_SETUP |= bADC_POWER_EN;
    ADC_CK_SE = FREQ_SYS / 8000000;
    ADC_CTRL &= ~MASK_ADC_CYCLE;
    ADC_CTRL &= ~(bADC_CHANN_MOD1 | bADC_CHANN_MOD0);
    ADC_EX_SW |= bADC_RESOLUTION;
    delay_us(100);
}

uint16_t adcGet(uint8_t channel) {
    channel &= 0x7;
    ADC_CHANN = 1 << channel;
    delay_us(10);
    ADC_CTRL |= bADC_SAMPLE;
    delay_us(50);
    ADC_CTRL &= ~bADC_SAMPLE;
    while ((ADC_STAT & bADC_IF_ACT) == 0);
    ADC_STAT |= bADC_IF_ACT;
    delay_us(10);
    ADC_CTRL |= bADC_SAMPLE;
    delay_us(50);
    ADC_CTRL &= ~bADC_SAMPLE;
    while ((ADC_STAT & bADC_IF_ACT) == 0);
    ADC_STAT |= bADC_IF_ACT;
    return ADC_FIFO;
}

void __pwm1_interrupt() __interrupt (INT_NO_PWM1) __using (2) {
    if (PWM_CTRL & bPWM_IF_END)
        PWM_CTRL |= bPWM_IF_END;
}

void pwmInit() {
    PWM_CK_SE = 2;
    PWM_CYCLE = 255;
    PIN_FUNC |= bPWM1_PIN_X;
    PWM_CTRL &= ~bPWM_CLR_ALL;
    PWM_CTRL &= ~bPWM_MOD_MFM;
    PWM_CTRL |=	bPWM_IE_END;
    PWM_CTRL |=	(bPWM_OUT_EN | bPWM2_OUT_EN);
    PWM_CTRL |= bPWM_IF_END;
    PWM_CTRL &= ~(bPWM_POLAR | bPWM2_POLAR);
    IE_PWM1 = 1;
}

void pwmSet(uint8_t ch, uint8_t duty) {
    if (ch == 1)
        PWM_DATA = duty;
    else if (ch == 2)
        PWM_DATA2 = duty;
}

#ifdef LED_ADR

__sbit __at (LED_ADR) LED_OUT;
#define RGB_CNT 78
static uint8_t rgbData[RGB_CNT][3];

void __ws_rst() {
    LED_OUT = 0;
    delay_us(500);
    LED_OUT = 1;
}

void __ws_send(uint8_t c) {
    c;
    __asm
    mov r2, #0x08
    mov a, dpl
head:
    rlc a
    jnc zero
    setb _LED_OUT
    mov r3, #0x0C
nzhead1:
    djnz r3, nzhead1
    clr _LED_OUT
    mov r3, #0x09
nzhead2:
    djnz r3, nzhead2
    djnz r2, head
    ret
zero:
    setb _LED_OUT
    mov r3, #0x04
zhead1:
    djnz r3, zhead1
    clr _LED_OUT
    mov r3, #0x09
zhead2:
    djnz r3, zhead2
    djnz r2, head
    ret
    __endasm;
}

void rgbSet(uint8_t index, uint32_t value) {
    uint8_t a = (value >> 24) & 0xFF;
    uint8_t r = (value >> 16) & 0xFF;
    uint8_t g = (value >> 8) & 0xFF;
    uint8_t b = value & 0xFF;

    r = (uint16_t) r * (uint16_t) a / 255;
    g = (uint16_t) g * (uint16_t) a / 255;
    b = (uint16_t) b * (uint16_t) a / 255;

    rgbSet3(index, r, g, b);
}

void rgbSet3(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    rgbData[index % RGB_CNT][0] = r;
    rgbData[index % RGB_CNT][1] = g;
    rgbData[index % RGB_CNT][2] = b;
}

void rgbPush() {
    E_DIS = 1;
    __ws_rst();
    uint8_t i;
    for (i = 0; i < RGB_CNT; i++) {
        __ws_send(rgbData[i][1]); // G
        __ws_send(rgbData[i][0]); // R
        __ws_send(rgbData[i][2]); // B
    }
    __ws_rst();
    E_DIS = 0;
}

#define abs__(v) ((v) < 0 ? -(v) : (v))
uint32_t hsv2RGB(uint16_t h, uint8_t s, uint8_t v) {
    h = h % 360;
    uint8_t c = (uint16_t) v * (uint16_t) s / 255;
    uint8_t x = (uint16_t) c * (255 - abs__((int16_t) (h * 255 / 60) % 511 - 255)) / 255;
    uint8_t m = v - c;
    uint8_t rt, gt, bt;
    if (h < 60) {
        rt = c; gt = x; bt = 0;
    } else if (h >= 60 && h < 120) {
        rt = x; gt = c; bt = 0;
    } else if (h >= 120 && h < 180) {
        rt = 0; gt = c; bt = x;
    } else if (h >= 180 && h < 240) {
        rt = 0; gt = x; bt = c;
    } else if (h >= 240 && h < 300) {
        rt = x; gt = 0; bt = c;
    } else {
        rt = c; gt = 0; bt = x;
    }
    
    return (uint32_t) (rt + m) << 16 | (uint32_t) (gt + m) << 8 | (bt + m);
}

#endif

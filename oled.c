#include "oled.h"
#include "font.h"

#include "ch559.h"

#define OLED_RST    P16
#define OLED_DC     P07
#define OLED_CS     P06

static uint16_t oled_buffer[FONT_MAX_WIDTH * 8 / 4];

/* f_w / f_h 是 uint16_t：位图宽度最大等于屏宽 256，uint8_t 装不下。
   pos / off 只在文字那条路上用（宽 ≤ FONT_MAX_WIDTH），保持 uint8_t。
   ⚠️ 一部分显式加了 __xdata：SDCC 的 --model-large **只管指针宽度**，变量的默认
   存储仍是 __data，而直接寻址区一共只有 128 字节 —— 本工程加上中文字库后就卡在
   这里（链接报 "Couldn't get N bytes ... for area OSEG"，因为函数局部量也抢这块）。
   所以把"每次绘制才碰一次"的量挪去 xdata，**把真正在像素/字节内层循环里的
   pos / off / seg / col / row / io 留在直接寻址区**，免得拖慢刷屏。 */
__xdata uint16_t f_w, f_h;
uint8_t pos, off;
__xdata uint16_t f;
/* 注意写法：__xdata 在 `*` 之后修饰的是**指针本身**（放 xdata）；
   写在类型前面修饰的会变成"指向 xdata 的指针"，SDCC 会报 error 47 类型不匹配。 */
const uint8_t* __xdata font;

__xdata uint8_t xs, ys, xe, ye;
__xdata uint8_t a, b, end, cnt;

uint8_t seg, col, row;
uint16_t io;

void __delay__() {
    SAFE_MOD++;
    SAFE_MOD++;
    SAFE_MOD++;
    SAFE_MOD++;
    SAFE_MOD++;

    SAFE_MOD++;
    SAFE_MOD++;
    SAFE_MOD++;
}

#define OLED_DO(v) do { \
        SPI0_DATA = v; \
        while (S0_FREE == 0); \
    } while (0)

void writeCmd0(uint8_t cmd) {
    OLED_CS = 0;
    OLED_DC = 0;
    OLED_DO(cmd);
    OLED_CS = 1;
}

void writeCmd1(uint8_t cmd, uint8_t dat) {
    OLED_CS = 0;
    OLED_DC = 0;
    OLED_DO(cmd);
    OLED_DC = 1;
    OLED_DO(dat);
    OLED_CS = 1;
}

void writeCmd2(uint8_t cmd, uint16_t dat) {
    OLED_CS = 0;
    OLED_DC = 0;
    OLED_DO(cmd);
    OLED_DC = 1;
    OLED_DO(dat & 0xFF);
    OLED_DO(dat >> 8);
    OLED_CS = 1;
}

void oledAddr(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    xs = x / 4;
    ys = y;
    xe = x / 4 + (w - 1) / 4;
    ye = y + h - 1;

    writeCmd2(0x15, (0x1C + xs) | ((0x1C + xe) << 8));
    writeCmd2(0x75, ys | (ye << 8));
}

void oledTransfer(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* ptr, uint8_t dat) {
    oledAddr(x, y, w, h);
    writeCmd0(0x5C);
    OLED_DC = 1;
    OLED_CS = 0;
    /* ⚠️ 字节数要按**地址窗口**算，不是按请求的 w。
       oledAddr() 里 xs = x/4、xe = x/4 + (w-1)/4 —— 列地址的横向粒度是 **4 个像素**，
       所以窗口实际宽度是 w 向上取整到 4 的倍数，每行 2*(xe-xs+1) 字节。
       原来按 w*h/2 算，w 不是 4 的倍数时就不够写，**缺的正好落在窗口最后一行**
       （条形图"最后一行右侧少几个像素"就是这么来的：填充宽度随数值变，很少是 4 的倍数）。
       纯色填充（ptr == NULL）多写的那几个像素与填充同色，看不出来，所以直接写满；
       位图数据是按"每行 w/2 字节"紧凑排的，只能按 w*h/2 读 —— **所以非 4 倍数宽的位图
       本来就会错位，得由调用方保证**（现有位图宽度都是 4 的倍数：logo 128、方向箭头 16、字模 8）。 */
    if (ptr == NULL)
        f = (uint16_t) ((uint16_t) (xe - xs + 1) * 2u * h);
    else
        f = (uint16_t) (w * h / 2);

    if (ptr == NULL)
        for (io = 0; io < f; io++)
            OLED_DO(dat);
    else
        for (io = 0; io < f; io++)
            OLED_DO(ptr[io]);
    OLED_CS = 1;
}

void oledInit() {
    P0_DIR |= (bUDCD | bURI); // P06->CS P07->DC
    P1_DIR |= bMISO; // P16->RST

    OLED_RST = 1;
    OLED_DC = 1;
    OLED_CS = 1;

    spi0Init(7); // 8MHz

    OLED_RST = 0;
    delay(300);
    OLED_RST = 1;
    delay(50);

    writeCmd1(0xFD, 0x12);
    writeCmd0(0xAE);
    writeCmd1(0xB3, 0x91);
    writeCmd1(0xCA, 0x3F);
    writeCmd1(0xA2, 0x00);
    writeCmd1(0xA1, 0x00);
    writeCmd1(0xA0, 0x14);
    writeCmd1(0xB5, 0x00);
    writeCmd1(0xAB, 0x01);
    writeCmd2(0xB4, 0xFDA0);
    writeCmd1(0xC1, 0xFF);
    writeCmd1(0xC7, 0x0F);
    writeCmd0(0xB9);
    writeCmd1(0xB1, 0xE2);
    writeCmd2(0xD1, 0x2082);
    writeCmd1(0xBB, 0x1F);
    writeCmd1(0xB6, 0x08);
    writeCmd1(0xBE, 0x07);
    writeCmd0(0xA4);
    delay(50);

    writeCmd0(0xAF);
    delay(100);

    oledClear(0x0);
    writeCmd0(0xA6);
}

void oledClear(uint8_t color) {
    color &= 0x0F;
    off = color << 4 | color;
    
    oledTransfer(0, 0, OLED_WIDTH, OLED_HEIGHT, NULL, off);
}

void oledLogo() {
    font = fontGet(0xFF);
    fontSize(font, &f_w, &f_h);

    oledClear(0x0);
    /* 位图数组开头是 FONT_BMP_HEADER 字节的宽高头部，像素数据在后面 ——
       这里必须跳过它，否则会把 0x21,0x40,0x00,0x30 当成头 4 个像素画出去。 */
    oledBmpC(OLED_WIDTH / 2, OLED_HEIGHT / 2, f_w, f_h, font + FONT_BMP_HEADER);
}

void oledBmp(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bmp) {
    oledTransfer(x, y, w, h, bmp, 0x00);
}

void oledBmpC(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bmp) {
    oledBmp(x - w / 2, y - h / 2, w, h, bmp);
}

const static uint8_t off_remap[] = { 4, 0, 12, 8 };

void oledBinary(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color, uint8_t* icon) {
    if (x > OLED_WIDTH - w) { x = 0; y += h; }

    for (seg = 0; seg < h / 8; seg++) {
        for (col = 0; col < w; col++) {
            off = col % 4;
            f = w * seg + col;
            for (row = 0; row < 8; row++) {
                pos = (w * row + col) / 4;
                oled_buffer[pos] &= ~(0xF << off_remap[off]);
                oled_buffer[pos] |= ((icon[f] & (1 << row)) ? color & 0xF : color >> 4) << off_remap[off];
            }
        }
        oledBmp(x, y + seg * 8, w, 8, (const uint8_t*) oled_buffer);
    }
}

void oledChar(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char c) {
    c -= ' ';
    font = fontGet(fid);
    fontSize(font, &f_w, &f_h);

    oledBinary(x, y, f_w, f_h, color, (uint8_t*) (font + c * f_w * f_h / 8));

    if (x > OLED_WIDTH - f_w) { x = 0; y += f_h; }
}

void oledString(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char* str) {
    cnt = 0;

    font = fontGet(fid);
    fontSize(font, &f_w, &f_h);
    
    while (str[cnt] != '\0') {
        if (y > OLED_HEIGHT - f_h) { x = 0; y = 0; oledClear(color >> 4); }

        switch (str[cnt]) {
            case '\n':
                cnt++; x = 0; y += f_h;
                continue;
            case 0x08:
                cnt++;
                if (x >= f_w) x -= f_w;
                else {
                    x = OLED_WIDTH - f_w;
                    y -= f_h;
                }
                oledChar(x, y, fid, color, ' ');
                continue;
            default:
                oledChar(x, y, fid, color, str[cnt]);
                x += f_w;
                if (x >= OLED_WIDTH - f_w) {
                    x = 0;
                    y += f_h;
                }
                cnt++;
                break;
        }
    }
}

void oledStringC(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char* str) {
    fontSize(fontGet(fid), &f_w, &f_h);
    for (end = 0; str[end] != '\0'; end++);
    oledString(x - end * f_w / 2, y, fid, color, str);
}

void oledRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color) {
    color &= 0x0F;
    off = color << 4 | color;
    oledTransfer(x, y, w, h, NULL, off);
}

void oledRectC(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color) {
    oledRect(x - w / 2, y - h / 2, w, h, color);
}

/* ============================================================================
 * 6x8 ASCII / 12x12 中文：共用"合成到 nibble 缓冲、再整块 oledTransfer"的核心
 *
 * ⚠️ 这段代码是**照着 8051 那 128 字节的直接寻址区写的**，别随手改回去：
 *    函数**参数和局部量**都吃那块（SDCC 叫 OSEG/DSEG），而本工程已经被挤满。
 *    最初把 blitGlyph 写成 9 参数 + 5 局部量，一加进去链接就报
 *    "Could not get 10 consecutive bytes in internal RAM for area DSEG"。
 *    现在：合成状态全部放文件作用域并显式 __xdata（进 xdata 不占直接区），
 *    blitGlyph() 变成**无参函数**。代价是不可重入 —— 单线程刷屏，够用。
 *    （注意写法：`uint8_t* __xdata p` 是"指针自己在 xdata"，
 *      `__xdata uint8_t* p` 会变成"指向 xdata 的指针"，SDCC 报 error 47。）
 * ========================================================================== */

/* ---- 合成状态（xdata）---- */
static uint8_t*       __xdata bl_buf;       /* 目标缓冲 */
static const uint8_t* __xdata bl_glyph;     /* 1 位字形 */
static uint16_t       __xdata bl_w;         /* 缓冲每行像素数 */
static uint16_t       __xdata bl_ox, bl_oy; /* 字形在缓冲里的像素偏移 */
static uint8_t        __xdata bl_gw, bl_gh; /* 字形宽高 */
static uint8_t        __xdata bl_fg, bl_bg; /* 前景/背景 nibble */
static uint8_t        __xdata bl_seg, bl_col, bl_row, bl_m;
static uint8_t*       __xdata bl_q;

/* 把 bl_glyph 合成进 bl_buf。布局同 oledBinary()：byte[gw*(row/8)+col] 的 bit[row%8]。
   偶数列写高 nibble（高半字节是左边那个像素）、奇数列写低 nibble。 */
static void blitGlyph(void) {
    bl_seg = 0;
    while (bl_seg < (uint8_t) ((bl_gh + 7u) / 8u)) {
        bl_col = 0;
        while (bl_col < bl_gw) {
            bl_m = bl_glyph[(uint16_t) bl_gw * bl_seg + bl_col];
            bl_row = 0;
            while (bl_row < 8u) {
                if ((uint16_t) (bl_seg * 8u + bl_row) >= bl_gh) break;  /* 最后一截不满 8 行 */
                bl_q = bl_buf + ((uint16_t) (bl_oy + bl_seg * 8u + bl_row) * (bl_w / 2u))
                              + ((uint16_t) (bl_ox + bl_col) >> 1);
                if (((bl_ox + bl_col) & 1u) == 0u)
                    *bl_q = (uint8_t) ((*bl_q & 0x0Fu)
                                       | ((bl_m & 0x01u) ? (uint8_t) (bl_fg << 4)
                                                         : (uint8_t) (bl_bg << 4)));
                else
                    *bl_q = (uint8_t) ((*bl_q & 0xF0u) | ((bl_m & 0x01u) ? bl_fg : bl_bg));
                bl_m = (uint8_t) (bl_m >> 1);
                bl_row++;
            }
            bl_col++;
        }
        bl_seg++;
    }
}

/* 整块铺背景（前后两个 nibble 都填 bg）。也把状态全放文件作用域，省直接寻址区。 */
static uint8_t  __xdata clr_v;
static uint16_t __xdata clr_i;
static void clearBuf(uint8_t* __xdata buf, uint16_t n, uint8_t bg) {
    clr_v = (uint8_t) ((bg << 4) | bg);
    for (clr_i = 0; clr_i < n; clr_i++) buf[clr_i] = clr_v;
}

/* ---- 6x8 ASCII：一次 8 个字（48 像素）----
   48 是 4 的倍数，所以窗口正好对齐，字与字之间不会互相擦。
   缓冲 48/2*8 = 192 字节，放 __xdata。 */
#define OLED_S6_CHARS   8u
#define OLED_S6_W       (6u * OLED_S6_CHARS)                /* 48 */
static __xdata uint8_t oled_buf6[(OLED_S6_W / 2u) * 8u];    /* 192 */
static __xdata uint8_t oled_buf12[(12u / 2u) * 12u];        /* 72 */

void oledString6(uint16_t x, uint16_t y, uint8_t color, const char* str) {
    static uint8_t  __xdata n, i, ch;
    static uint16_t __xdata cx;
    const uint8_t* f6 = fontGet(0x02);              /* font_6x8，见 font.c */

    cx = x;
    while (*str) {
        for (n = 0; n < OLED_S6_CHARS && str[n]; n++) ;     /* 这一块几个字 */
        clearBuf(oled_buf6, (uint16_t) sizeof(oled_buf6), (uint8_t) (color >> 4));
        for (i = 0; i < n; i++) {
            ch = (uint8_t) str[i];
            if (ch < 0x20u || ch > 0x7Bu) ch = 0x20u;       /* 表外画空格：font_6x8 只到 0x7B */
            bl_buf = oled_buf6; bl_glyph = f6 + (uint16_t) (ch - 0x20u) * 6u;
            bl_w = OLED_S6_W;   bl_ox = (uint16_t) (i * 6u); bl_oy = 0;
            bl_gw = 6; bl_gh = 8;
            bl_fg = (uint8_t) (color & 0x0F); bl_bg = (uint8_t) (color >> 4);
            blitGlyph();
        }
        oledTransfer(cx, y, OLED_S6_W, 8, oled_buf6, 0);
        cx += OLED_S6_W;
        str += n;
    }
}

/* ---- 12x12 中文：单个字一次 ----
   12 像素宽是 4 的倍数，窗口正好对齐；6 字节/行 × 12 行 = 72 字节。
   生成的 fontzh_data.c 里每字 24 字节（12 列 × 2 段，第二段只用低 4 位）。 */
void oledCJK12(uint16_t x, uint16_t y, uint8_t color, const uint8_t* glyph) {
    clearBuf(oled_buf12, (uint16_t) sizeof(oled_buf12), (uint8_t) (color >> 4));
    bl_buf = oled_buf12; bl_glyph = glyph;
    bl_w = 12u; bl_ox = 0; bl_oy = 0; bl_gw = 12u; bl_gh = 12u;
    bl_fg = (uint8_t) (color & 0x0F); bl_bg = (uint8_t) (color >> 4);
    blitGlyph();
    oledTransfer(x, y, 12, 12, oled_buf12, 0);
}
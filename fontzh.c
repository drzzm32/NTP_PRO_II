#include "fontzh.h"

#include "sys.h"
#include "oled.h"

/* ============================================================================
 * 中文子集字库：查表 + UTF-8 混排绘制
 *
 * 点阵数据在 fontzh_data.c（生成的）。这里只管怎么找、怎么画。
 *
 * ⚠️ 这段代码是**照着 8051 的直接寻址区（128 字节）来写的**，别随手改回去：
 *    `--model-large` 下未加限定符的**文件作用域**变量进 xdata，而**函数局部量与
 *    参数**进那块只有 128 字节的直接寻址区（SDCC 叫 OSEG）。本工程里那块已经被
 *    oled/main/sys 几个模块占满，之前因为多了一层 utf8Next() 调用帧 + 几个局部
 *    指针，链接直接报 "Couldn't get 13 bytes ... for area OSEG"。
 *    所以这里：解码就地展开（不另开函数）、解码状态全部放文件作用域的静态量。
 *    代价是不可重入 —— 只有主循环单线程会调它，够用。
 * ========================================================================== */

/* 按码位查字形。字表只有几十个字，**线性扫就够了** —— 比二分少一堆 code 空间，
 * 而且最坏也就几十次比较，对 5Hz 的刷屏完全无所谓。 */
const uint8_t* fontzhGet(uint16_t code) {
    uint16_t i;

    for (i = 0; i < fontzh_count; i++)
        if (fontzh_codes[i] == code)
            return fontzh_bits + (uint16_t) (i * FONTZH_BYTES);   /* 12x12 时是 24，不是 W*H/8=18 */

    return 0;
}

/* ASCII 字库的字高（用 fid 区分：0 = font_8x8，1 = font_8x16） */
static uint8_t asciiFontH(uint8_t fid) {
    return fid ? 16u : 8u;
}

/* UTF-8 走位状态。放文件作用域是为了避开直接寻址区，理由见文件头。 */
static const char*    zh_p;     /* 当前解析到的位置 */
static const uint8_t* zh_q;     /* 当前字符的续字节游标 */
static uint16_t       zh_c;     /* 当前码位 */
static uint8_t        zh_off;   /* 中文在行内的垂直居中偏移 */

void oledStringZh(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, const char* str) {
    const uint8_t* g;

    zh_p = str;
    while (*zh_p) {
        if (*zh_p == '\n') {                        /* 换行：回行首、下移一行 */
            zh_p++;
            x = 0;
            y += (uint16_t) (asciiFontH(fid) + 2u);
            continue;
        }

        /* ---- 就地解一个 UTF-8 字符。
           不认识的编码给 0xFFFD 并把续字节跳掉 —— 至少不会卡住或越界。 ---- */
        zh_q = (const uint8_t*) zh_p;
        zh_c = *zh_q++;

        if (zh_c < 0x80u) {
            /* 单字节 ASCII，原样 */
        } else if ((zh_c & 0xE0u) == 0xC0u) {       /* 2 字节 */
            zh_c = (uint16_t) (((zh_c & 0x1Fu) << 6) | (uint16_t) (*zh_q++ & 0x3Fu));
        } else if ((zh_c & 0xF0u) == 0xE0u) {       /* 3 字节 —— 中文都在这一档 */
            zh_c = (uint16_t) (((uint16_t) (zh_c & 0x0Fu) << 12)
                             | ((uint16_t) (zh_q[0] & 0x3Fu) << 6)
                             |  (uint16_t) (zh_q[1] & 0x3Fu));
            zh_q += 2;
        } else {                                    /* 4 字节：本字库用不到，整段跳过 */
            while ((*zh_q & 0xC0u) == 0x80u) zh_q++;
            zh_c = 0xFFFDu;
        }
        zh_p = (const char*) zh_q;

        if (zh_c < 0x80u) {                         /* ASCII 交给原字库 */
            oledChar(x, y, fid, color, (char) zh_c);
            x += 8;
            continue;
        }

        g = fontzhGet(zh_c);
        if (g) {
            /* 8x8 的中文在行内垂直居中：fid=1（16 高）时上下各留 4px，和 ASCII 对齐；
               fid=0（8 高）时正好贴顶。 */
            zh_off = (uint8_t) ((asciiFontH(fid) - FONTZH_H) / 2u);
            oledBinary(x, (uint16_t) (y + zh_off), FONTZH_W, FONTZH_H,
                       color, (uint8_t*) g);
        }
        x += FONTZH_W;                              /* 缺字也占一格，免得整行错位 */
    }
}

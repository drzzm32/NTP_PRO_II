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

/* ---- UTF-8 走位与排版状态 ----
   放文件作用域：8051 的直接寻址区只有 128 字节，**函数参数和局部量都吃那块**，
   而本工程已占满（详见 oled.c 里 blitGlyph 那段）。不可重入，单线程刷屏够用。 */
static const char*    zh_p;     /* 当前解析位置 */
static const uint8_t* zh_q;     /* 当前字符之后的游标 */
static uint16_t       zh_c;     /* 当前码位 */
static uint16_t       zh_cx;    /* 当前笔位 */
static uint8_t        zh_pass, zh_run, zh_top, zh_dy, zh_w;

/* 行内排版：中文在 16px 的行里垂直居中（上下各留 (16-H)/2），ASCII 6x8 再往下偏
   FONTZH_DY (=3) —— 这样两者基线才对得上（12x12 中文基线约在第 10 行、
   6x8 ASCII 在第 7 行，差 3）。FONTZH_DY 由生成的头文件给出，见 fontzh_data.h。 */
void oledStringZh(uint16_t x, uint16_t y, uint8_t color, const char* str) {
    const uint8_t* g;

    zh_top = (uint8_t) ((16u - FONTZH_H) / 2u);
    zh_dy  = (uint8_t) (zh_top + FONTZH_DY);

    /* 两趟画：**先 ASCII、后中文**。
       6x8 的 ASCII 一次要发 4 的倍数宽（最多比实际内容多 3 像素），这些多出来的
       像素会盖到右边；中文的 FONTZH_W 宽格子正好落在自己的范围内，后画就能把那些
       越界像素盖回来。反过来（先中文后 ASCII）就会把中文擦掉。 */
    for (zh_pass = 0; zh_pass < 2u; zh_pass++) {
        zh_cx = x;
        zh_p = str;
        while (*zh_p) {
            if (*zh_p == '\n') {                    /* 换行：回行首、下移一行 */
                zh_p++;
                zh_cx = 0;
                y += 16u;
                continue;
            }

            /* ---- 就地解一个 UTF-8 字符（不另开函数，省一层调用帧）---- */
            zh_q = (const uint8_t*) zh_p;
            zh_c = *zh_q++;
            if (zh_c < 0x80u) {
                /* 单字节 ASCII，原样 */
            } else if ((zh_c & 0xE0u) == 0xC0u) {   /* 2 字节 */
                zh_c = (uint16_t) (((zh_c & 0x1Fu) << 6) | (uint16_t) (*zh_q++ & 0x3Fu));
            } else if ((zh_c & 0xF0u) == 0xE0u) {   /* 3 字节 —— 中文都在这档 */
                zh_c = (uint16_t) (((uint16_t) (zh_c & 0x0Fu) << 12)
                                 | ((uint16_t) (zh_q[0] & 0x3Fu) << 6)
                                 |  (uint16_t) (zh_q[1] & 0x3Fu));
                zh_q += 2;
            } else {                                /* 4 字节：本字库用不到，整段跳过 */
                while ((*zh_q & 0xC0u) == 0x80u) zh_q++;
                zh_c = 0xFFFDu;
            }

            if (zh_c < 0x80u) {
                /* 攒一段连续的 ASCII 再整块画（oledString6N 一次最多 8 个字） */
                zh_run = 1u;
                while (zh_run < 8u && zh_q[zh_run - 1u]
                       && (uint8_t) zh_q[zh_run - 1u] < 0x80u)
                    zh_run++;

                /* 推进量必须是**补齐到 4 的倍数**后的宽度，和 oledString6N 实际画出去
                   的一样：否则后面接的中文会落在非 4 对齐的 x 上，地址窗口（按 x/4
                   对齐）整体偏移，画面错位。 */
                zh_w = (uint8_t) ((((6u * zh_run) + 3u) / 4u) * 4u);
                if (zh_pass == 0u)
                    oledString6N(zh_cx, (uint16_t) (y + zh_dy), color,
                                 (const char*) (zh_q - 1), zh_run);

                zh_cx += zh_w;
                zh_p = (const char*) (zh_q - 1 + zh_run);   /* 整段跳过；两趟步长一致 */
                continue;
            }

            /* ---- 中文 ---- */
            if (zh_pass != 0u) {
                g = fontzhGet(zh_c);
                if (g) oledCJK12(zh_cx, (uint16_t) (y + zh_top), color, g);
            }
            zh_cx += FONTZH_W;                      /* 缺字也占一格，免得整行错位 */
            zh_p = (const char*) zh_q;
        }
    }
}

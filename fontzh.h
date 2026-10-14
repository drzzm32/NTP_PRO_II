#ifndef __FONTZH_H
#define __FONTZH_H

#include <stdint.h>
#include "fontzh_data.h"   /* 生成的：FONTZH_W/H/BYTES/DY */

/* ============================================================================
 * 中文点阵子集（8x8）
 *
 * 源字体 fusion-pixel-8px-monospaced-zh_hans.ms.bitmap.ttf 是**纯内嵌点阵**字体
 * （只有 EBDT/EBLC，glyf 是空的），有 27689 个字形、按 8 字节点阵算约 221KB。
 * CH559 的 code 空间只有 60KB，整份放不下 —— 所以只取固件真正用到的字。
 *
 * ⚠️ fontzh_data.c 是 tools/make_fontzh.py 生成的，**别手改**。
 *    加字的办法：在 .c 的**字符串字面量**里写中文（注释里的中文不算），然后
 *    重新跑 `python tools/make_fontzh.py`。
 *
 * ⚠️ 只有 8x8 这一档：源字体只内嵌了 8ppem 的点阵、没有轮廓可缩放，放大渲染
 *    会得到空白。所以中文永远是 8x8，要和 8x16 的 ASCII 混排时靠垂直居中对策。
 * ========================================================================== */


/* 生成的表，见 fontzh_data.c */
extern const uint16_t fontzh_count;
extern const uint16_t fontzh_codes[];   /* 码位，升序 */
extern const uint8_t  fontzh_bits[];    /* 每字 FONTZH_BYTES 字节（见 fontzh_data.h） */

/* 按码位查字形，返回 8 字节的 1 位点阵（布局同 oledBinary()）；查不到返回 0 */
const uint8_t* fontzhGet(uint16_t code);

/* 画一行混排字符串（UTF-8）：
 *   * 非 ASCII 走中文子集（8x8），并按 fid 对应的 ASCII 字高**垂直居中**
 *     —— fid=0（8x8）时对齐顶部，fid=1（8x16）时上下各留 4px，这样中英混排能对齐；
 *   * 其余字符交给 ASCII 字库，fid 就是 fontGet() 的字体号；
 *   * 字库里没有的中文占一格宽度、不画东西，免得整行错位。
 * 支持 '\n' 换行（回行首、下移 10px）。 */
void oledStringZh(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, const char* str);

#endif

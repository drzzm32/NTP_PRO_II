/* 本文件由 tools/make_fontzh.py 生成 —— 别手改！
 * 只放尺寸：fontzh.h include 它，这样换字体档位（8px/12px）不用手改代码。 */
#ifndef __FONTZH_DATA_H
#define __FONTZH_DATA_H

#define FONTZH_W     12
#define FONTZH_H     12
#define FONTZH_SEGS  2          /* 每字占几个 8 行段 */
#define FONTZH_BYTES 24          /* 每字字节数 = W * SEGS */

/* 与 12x12 中文混排时，6x8 ASCII 要下移这么多像素才是基线对齐：
 * CJK 基线约在第 10 行、ASCII 在第 7 行，差 3。见 oledStringZh()。 */
#define FONTZH_DY    3

#endif
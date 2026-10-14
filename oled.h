#ifndef __OLED_H
#define __OLED_H

#include "sys.h"

#define OLED_WIDTH  256
#define OLED_HEIGHT 64

void oledInit();
void oledClear(uint8_t color);
void oledLogo();
void oledBmp(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bmp);
void oledBmpC(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bmp);
// 以下的color为前景背景组合参数, color[3:0]为前景, color[7:4]为背景
void oledBinary(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color, uint8_t* icon);
void oledChar(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char c);
void oledString(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char* str);
void oledStringC(uint16_t x, uint16_t y, uint8_t fid, uint8_t color, char* str);
void oledRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
void oledRectC(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);

/* ---- 6x8 ASCII 与 12x12 中文：先在 RAM 里拼好、再整块发 ----
   为什么不能用 oledString(): 它走 oledBinary()，而 oledBinary() 按
   f = W*(row/8)+col、pos = (W*row+col)/4 定位，**要求每行正好 W/4 个字**
   （即 W 是 4 的倍数）。6x8 的 W=6 不满足；12x12 宽度满足但高 12 不是 8 的倍数。
   另外列地址横向粒度是 4 像素（见 oledAddr），6 像素步进的字若各自成一个窗口，
   相邻字的窗口会互相擦掉 —— 所以只能整串拼好再一次发出。

   ⚠️ 起点 x 必须落在 4 的倍数上（窗口按 4 对齐）。oledString6() 每块固定 48 像素
      （8 字 × 6），也是 4 的倍数，所以连续调用不会错位。
   ⚠️ oledString6() 一块写满 48 像素宽，块尾空白会覆盖右侧 —— 右边还有内容时
      要自己留够余量。 */
void oledString6(uint16_t x, uint16_t y, uint8_t color, const char* str);
/* 只画前 n 个字符（n<=8），发出去的宽度按 4 的倍数补齐 —— 中英混排时必须用这个：
   固定发 48 像素会在串尾多擦掉最多 48 像素，把右边的东西清掉。
   **返回实际发出的宽度**（补齐到 4 的倍数）：调用方要用它推进笔位，别自己再算
   一遍 —— 那个公式一旦和这里不一致，后面接的中文就会落在非 4 对齐的 x 上。 */
uint16_t oledString6N(uint16_t x, uint16_t y, uint8_t color, const char* str, uint8_t n);
void oledCJK12(uint16_t x, uint16_t y, uint8_t color, const uint8_t* glyph);
#endif

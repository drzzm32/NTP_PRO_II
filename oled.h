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

#endif

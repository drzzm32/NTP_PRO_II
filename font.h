#ifndef __FONT_H
#define __FONT_H

#include <stdint.h>

#define FONT_MAX_WIDTH   16 // 用于建立一个8像素高的局部缓冲区

const uint8_t* fontGet(uint8_t id);
void fontSize(const uint8_t* font, uint8_t* w, uint8_t* h);

#endif

#ifndef __FONT_H
#define __FONT_H

#include <stdint.h>

#define FONT_MAX_WIDTH   16 // 用于建立一个8像素高的局部缓冲区

/* 4 位灰度位图数组开头的头部长度（0x21 + 宽16位小端 + 高8位）。
   位图数组 = [这 4 字节头部][纯像素数据]，所以：
     * fontSize() 从 font[0..3] 读宽高；
     * 画图必须从 **font + FONT_BMP_HEADER** 开始，否则会把头部当像素画出去。
   见 font.c 的 logo[]、oled.c 的 oledLogo()。 */
#define FONT_BMP_HEADER  4

const uint8_t* fontGet(uint8_t id);

/* 取字体/位图的宽高。
   ⚠️ w/h 是 **uint16_t**（原来是 uint8_t）：位图宽度最大等于屏宽 256，
   一个字节装不下。位图数组开头那 4 字节头部就是给这个函数读的。 */
void fontSize(const uint8_t* font, uint16_t* w, uint16_t* h);

#endif

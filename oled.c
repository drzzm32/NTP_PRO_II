#include "oled.h"
#include "font.h"

#include "ch559.h"

#define OLED_RST    P16
#define OLED_DC     P07
#define OLED_CS     P06

static uint16_t oled_buffer[FONT_MAX_WIDTH * 8 / 4];

const uint8_t* font;
uint8_t f_w, f_h, pos, off;
uint16_t f;

uint8_t xs, ys, xe, ye;
uint8_t a, b, end, cnt;

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
    f = w * h / 2;
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
    oledBmpC(OLED_WIDTH / 2, OLED_HEIGHT / 2, f_w, f_h, font);
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

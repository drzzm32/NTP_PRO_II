#ifndef __SYS_H
#define __SYS_H

#include <stdint.h>
#include <stddef.h>

#define FREQ_SYS    56000000

void sysPinConfig();
void sysClockConfig();
void delay_us(uint16_t n);
void delay(uint16_t n);

void mset(uint8_t* ptr, uint8_t val, size_t siz);
void mmov(uint8_t* dst, uint8_t* src, size_t siz);

void uart0Config(uint32_t baud);
void uart0Send(uint8_t data);
void uart0Write(char* s);
uint8_t uart0Get();
uint8_t uart0Gets(uint8_t* dat, uint8_t len);
uint16_t uart0BlockRecv(uint8_t* ptr, uint16_t siz);

void adcInit();
uint16_t adcGet(uint8_t channel);

void pwmInit();
void pwmSet(uint8_t ch, uint8_t duty);

#ifdef LED_ADR
void rgbSet(uint8_t index, uint32_t value);
void rgbSet3(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void rgbPush();
uint32_t hsv2RGB(uint16_t h, uint8_t s, uint8_t v);
#endif

#endif

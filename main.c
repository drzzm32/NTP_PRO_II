#include "ch559.h"

#include <stdio.h>
char buff[32];

#include "sys.h"
#include "pin.h"
#include "usb.h"
#include "dac.h"
#include "oled.h"

void __usb_interrupt() __interrupt (INT_NO_USB) __using (1);
extern __bit USB_OK;
void __pwm1_interrupt() __interrupt (INT_NO_PWM1) __using (2);

__bit PWM_MODE = false;

void main() {
    LED = 1;
    MOT_F = 0; MOT_R = 0;
    sysPinConfig();
    sysClockConfig();
    delay(10);

    oledInit();
    oledLogo();
    delay(1000);

    PWM_MODE = (KEY_1 == 0);
    if (PWM_MODE)
        pwmInit();

    adcInit();
    dacInit();
    usbDevInit();
    EA = 1;

    oledClear(0x0);
    oledStringC(128, 32, 1, 0x0F, "NTP PRO II");
    
    delay(50);
    releaseAllKey();
    pushKeyData();
    requestHIDData();

    uint16_t ad_val;

    int16_t isen = 0, vout = 0;

    uint8_t p_u = 0, p_d = 0, p_r = 0;
    int8_t dir_raw, dir = 0;

    uint16_t v_off = 0, pwr = 0;

    uint8_t p_btn = 0;
    int16_t p_dl = 0, p_dr = 0;

    while (1) {
        ad_val = adcGet(ISEN) & 0x7FF;
        ad_val = ad_val * 3300 / 2047;
        isen = ((int16_t) ad_val - 1650) * 1000 / 132 / 4;

        ad_val = adcGet(AIN_1) & 0x7FF;
        dir_raw = (int8_t) (ad_val / 700) - 1;

        if (!p_u && dir_raw == 1) {
            p_u = 1;
            dir += 1;
            dir = dir > 1 ? 1 : dir;
        } else if (p_u && dir_raw != 1) {
            p_u = 0;
        }

        if (!p_d && dir_raw == -1) {
            p_d = 1;
            dir -= 1;
            dir = dir < -1 ? -1 : dir;
        } else if (p_d && dir_raw != -1) {
            p_d = 0;
        }

        if (!USB_OK) {
            if (!p_r && KEY_1 == 0) {
                p_r = 1;
                dir = 0;
            } else if (p_r && KEY_1 != 0) {
                p_r = 0;
            }
        }

        switch (dir) {
            case -1:
                PRT_0 = 0; PRT_1 = 0; PRT_2 = 1;
                break;
            case 0:
                PRT_0 = 0; PRT_1 = 1; PRT_2 = 0;
                break;
            case 1:
                PRT_0 = 1; PRT_1 = 0; PRT_2 = 0;
                break;
            default:
                PRT_0 = 1; PRT_1 = 1; PRT_2 = 1;
                break;
        }

        v_off = adcGet(AIN_2) & 0x7FF;
        pwr = adcGet(AIN_3) & 0x7FF;

        if (USB_OK) {
            uint8_t btn = 0;
            btn |= (KEY_1 == 0 ? 0x01 : 0x00);
            btn |= (KEY_2 == 0 ? 0x02 : 0x00);
            btn |= (KEY_3 == 0 ? 0x04 : 0x00);
            btn |= (dir == 1   ? 0x10 : 0x00);
            btn |= (dir == -1  ? 0x20 : 0x00);

            int16_t d_l = v_off - 1023;
            d_l = d_l < -1023 ? -1023 : d_l;
            d_l = d_l > 1023 ? 1023 : d_l;

            int16_t d_r = pwr - 1023;
            d_r = d_r < -1023 ? -1023 : d_r;
            d_r = d_r > 1023 ? 1023 : d_r;

            releaseAllKey();
            setKeycode(0, 2);
            setKeycode(1, btn);
            setKeycode(2, (uint8_t) (((uint16_t) d_l) & 0xFF));
            setKeycode(3, (uint8_t) (((uint16_t) d_l) >> 8));
            setKeycode(4, (uint8_t) (((uint16_t) d_r) & 0xFF));
            setKeycode(5, (uint8_t) (((uint16_t) d_r) >> 8));

            if (p_btn != btn || p_dl != d_l || p_dr != d_r) {
                p_btn = btn; p_dl = d_l; p_dr = d_r;
                pushKeyData();
            } else {
                sprintf(buff, "D%dP%dV%dB%02X\n", dir, pwr >> 3, v_off >> 3, btn);
                //sprintf(buff, "Vo:%2d.%1dV, Is:%4dmA\n", vout / 1000, (vout % 1000) / 100, isen);
                hidPrint(buff);
                delay(100);
            }
        } else {
            if (PWM_MODE) {
                uint8_t pwm = pwr >> 3;
                switch (dir) {
                    case -1:
                        pwmSet(1, 0); pwmSet(2, pwr);
                        break;
                    case 0:
                        pwmSet(1, 0); pwmSet(2, 0);
                        break;
                    case 1:
                        pwmSet(1, pwr); pwmSet(2, 0);
                        break;
                    default:
                        pwmSet(1, 255); pwmSet(2, 255);
                        break;
                }

                vout = v_off * 6;
                voltSet(v_off * 6);
            } else {
                switch (dir) {
                    case -1:
                        MOT_F = 0; MOT_R = 1;
                        break;
                    case 0:
                        MOT_F = 0; MOT_R = 0;
                        break;
                    case 1:
                        MOT_F = 1; MOT_R = 0;
                        break;
                    default:
                        MOT_F = 1; MOT_R = 1;
                        break;
                }

                vout = pwr * 6 + v_off;
                voltSet(pwr * 6 + v_off);
            }
            delay(20);
        }
    }
}

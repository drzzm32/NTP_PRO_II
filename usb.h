#ifndef __USB_H
#define __USB_H

#include <stdint.h>

/* 厂商自定义接口(接口1)一次传输的数据字节数。
   CustomRepDesc 里的 Report Count 和 HIDInput/HIDOutput 的尺寸都由它决定。 */
#define HID_BUF_SIZE 42

/* 初始化 CH559 的 USB 设备控制器并接入总线。
   这是本头文件唯一对外的东西：业务层要用的手柄上报接口都在 joy.h 里
   （joyReady/joySend/joySendIfChanged/joyInvalidate/joyPrint/... 由 usb.c 实现）。 */
void usbDevInit();

#endif

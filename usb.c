# include "usb.h"

#include <stdlib.h>
#include <string.h>

#include "ch559.h"
#include "sys.h"
#include "pin.h"
#include "joy.h"

#define THIS_ENDP0_SIZE DEFAULT_ENDP0_SIZE

/* 手柄输入报表长度：2字节按键(只用15位) + 1字节hat+填充 + 4轴×2字节 X/Y/Z/Rz
   = 11 字节，无 Report ID。必须与 JoyRepDesc 一致。 */
#define JOY_REPORT_SIZE 11

/* EP1 IN 的 wMaxPacketSize，必须与 usbCfgDesc 里端点0x81的 wMaxPacketSize 一致。
   一帧报表超过这个长度主机不会接受，所以发送前要挡住。 */
#define EP1_IN_MAX_PACKET 16

const uint8_c usbDevDesc[] = {
    0x12,               // 描述符长度(18字节)
    0x01,               // 描述符类型
    0x00, 0x02,         // 本设备所用USB版本(2.0)
    0x00,               // 类代码
    0x00,               // 子类代码
    0x00,               // 设备所用协议
    THIS_ENDP0_SIZE,    // 端点0最大包长
    0x32, 0x32,         // 厂商ID
    0x60, 0x00,         // 产品ID, NSDN-PCB 0096
    0x00, 0x01,         // 设备版本号 (1.0)
    0x01,               // 描述厂商信息的字符串描述符的索引值
    0x02,               // 描述产品信息的字串描述符的索引值
    0x03,               // 描述设备序列号信息的字串描述符的索引值
    0x01                // 可能的配置数
};

const uint8_c usbCfgDesc[] = {
    0x09,        //   bLength
    0x02,        //   bDescriptorType (Configuration)
    0x42, 0x00,  //   wTotalLength 66（删掉接口0的OUT端点后少了7字节）
    0x02,        //   bNumInterfaces 2
    0x01,        //   bConfigurationValue
    0x04,        //   iConfiguration (String Index)
    0xA0,        //   bmAttributes Remote Wakeup
    0xFA,        //   bMaxPower 500mA

/* -------------------------------- */
//  Game Pad（接口0）：单一 Game Pad 应用集合，无 Report ID，无输出报表
    0x09,        //   bLength
    0x04,        //   bDescriptorType (Interface)
    0x00,        //   bInterfaceNumber 0
    0x00,        //   bAlternateSetting
    0x01,        //   bNumEndpoints 1（手柄没有输出报表；键盘的 EP1 OUT 已随键盘接口删除）
    0x03,        //   bInterfaceClass
    0x00,        //   bInterfaceSubClass (0=非boot。原来是0x01=boot键盘，和16字节报表自相矛盾)
    0x00,        //   bInterfaceProtocol (0=非boot。原来0x01=键盘，主机切boot协议就会错帧)
    0x05,        //   iInterface (String Index)

    0x09,        //   bLength
    0x21,        //   bDescriptorType (HID)
    0x11, 0x01,  //   bcdHID 1.11
    0x00,        //   bCountryCode
    0x01,        //   bNumDescriptors
    0x22,        //   bDescriptorType[0] (HID)
    // 注意：这是手写的常量，不会随 sizeof(JoyRepDesc) 自动更新！
    // 改动 JoyRepDesc 后必须回来改这里，否则主机会读到被截断的报表描述符。
    // （文件末尾有一条编译期断言替我们看着这两处是否同步。）
    0x50, 0x00,  //   wDescriptorLength[0] 80 = sizeof(JoyRepDesc)

    0x07,        //   bLength
    0x05,        //   bDescriptorType (Endpoint)
    0x81,        //   bEndpointAddress (IN/D2H)
    0x03,        //   bmAttributes (Interrupt)
    0x10, 0x00,  //   wMaxPacketSize 16（必须与 usb.c 里 EP1_IN_MAX_PACKET 一致）
    0x01,        //   bInterval 1 (unit depends on device speed)

/* -------------------------------- */
//  Custom HID
    0x09,        //   bLength
    0x04,        //   bDescriptorType (Interface)
    0x01,        //   bInterfaceNumber 1
    0x00,        //   bAlternateSetting
    0x02,        //   bNumEndpoints 2
    0x03,        //   bInterfaceClass
    0x00,        //   bInterfaceSubClass
    0x00,        //   bInterfaceProtocol
    /* iInterface = 0：本接口**不再提供接口名字符串**。
       Windows/DirectInput 判定"这个游戏控制器叫什么名字"的缓存键是
       HKLM/HKCU\System\CurrentControlSet\Control\MediaProperties\PrivateProperties\
       Joystick\OEM\VID_xxxx&PID_xxxx —— **只有 VID&PID，没有接口号**。两个 HID 接口
       各报一个不同的 iInterface 时，谁最后写进那个槽位谁就赢：实测 NGB-PAD 的机器级
       缓存里存成了本接口的 "NSDN Custom"，于是 joy.cpl 里手柄顶着自定义接口的名字。
       这里置 0，整个设备就只剩接口0 一个名字，从根上消掉这个歧义。
       注：置 0 之后字符串索引 6 就没人引用了，usbCusStrDesc 与 GET_DESCRIPTOR 的
       case 6 已一并删除。 */
    0x00,        //   iInterface (String Index) = 0（无接口名字符串）

    0x09,        //   bLength
    0x21,        //   bDescriptorType (HID)
    0x10, 0x01,  //   bcdHID 1.10
    0x00,        //   bCountryCode
    0x01,        //   bNumDescriptors
    0x22,        //   bDescriptorType[0] (HID)
    0x24, 0x00,  //   wDescriptorLength[0] 36

    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x83,        // bEndpointAddress (IN/D2H)
    0x03,        // bmAttributes (Interrupt)
    0x40, 0x00,  // wMaxPacketSize 64
    0x01,        // bInterval 1（全速中断端点的bInterval合法范围是1..255，原来的0非法）

    0x07,        // bLength
    0x05,        // bDescriptorType (Endpoint)
    0x03,        // bEndpointAddress (OUT/H2D)
    0x03,        // bmAttributes (Interrupt)
    0x40, 0x00,  // wMaxPacketSize 64
    0x01,        // bInterval 1（同EP3 IN，原来的0非法）
};

/*HID类报表描述符：单一 Game Pad 应用集合，80(0x50)字节，无 Report ID
  载荷固定 11 字节：[0..1]按键1..15 [2]hat低4位 [3..4]X [5..6]Y [7..8]Z [9..10]Rz
  各轴为 16 位字段、小端补码，逻辑范围 -1023..+1023、中点 0（= ADC 的 11 位有效
  分辨率，见 joy.h 的"模拟轴"）
  改这个数组必须同步改 usbCfgDesc 里硬编码的 wDescriptorLength[0]=0x50 */
const uint8_c JoyRepDesc[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x0F,        //   Usage Maximum (15)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0F,        //   Report Count (15)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x01,        //   Report Size (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)      -- 1 bit padding
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x07,        //   Logical Maximum (7)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x65, 0x14,        //   Unit (Eng Rot: degree)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null State)
    0x65, 0x00,        //   Unit (None)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x01,        //   Input (Const,Array,Abs)    -- 4 bit padding
    0x05, 0x01,        //   Usage Page (Generic Desktop Ctrls)
    0x16, 0x01, 0xFC,  //   Logical Minimum (-1023)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x04,        //   Report Count (4)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x35,        //   Usage (Rz)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0xC0,              // End Collection
};

/* 编译期核对：本数组长度必须和上面 usbCfgDesc 里手写的 wDescriptorLength[0] 一致。
   这两处不同步会让主机读到被截断的报表描述符，是个已经踩过、光靠注释守不住的坑，
   所以让编译器替我们盯着：改了 JoyRepDesc 之后这里会**直接编译报错**
   （error 195: Size of array 'joy_rep_desc_size_check' is negative），
   按提示把 wDescriptorLength[0] 同步改成 sizeof(JoyRepDesc)。
   为什么不用 _Static_assert：SDCC 4.5 对它只报 warning 215、编译照过，挡不住这个坑；
   "条件为假就定义负长度数组"在这里是硬错误，才拦得住。 */
typedef char joy_rep_desc_size_check[(sizeof(JoyRepDesc) == 0x50) ? 1 : -1];

const uint8_c CustomRepDesc[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0xAA,        //   Report ID (170)
    0x95, HID_BUF_SIZE,//   Report Count (XX)
    0x75, 0x08,        //   Report Size (8)
    0x25, 0x01,        //   Logical Maximum (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x09, 0x01,        //   Usage (0x01)
    0x81, 0x02,        //   Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
    0x85, 0x55,        //   Report ID (85)
    0x95, HID_BUF_SIZE,//   Report Count (XX)
    0x75, 0x08,        //   Report Size (8)
    0x25, 0x01,        //   Logical Maximum (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x09, 0x01,        //   Usage (0x01)
    0x91, 0x02,        //   Output (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
    0xC0,              // End Collection
};

const uint8_c usbLangDesc[] = { 0x04, 0x03, 0x04, 0x08 };
const uint8_c usbManuDesc[] = { 0x0A, 0x03, 'N', 0, 'S', 0, 'D', 0, 'N', 0 };
const uint8_c usbProdDesc[] = { 0x16, 0x03, 'N', 0,'T', 0,'P', 0, ' ', 0, 'P', 0, 'R', 0, 'O', 0, ' ', 0, 'I', 0, 'I', 0 };
const uint8_c usbSerialDesc[] = { 0x0A, 0x03, '0', 0, '0', 0, '0', 0, '0', 0 };
const uint8_c usbCfgStrDesc[] = { 0x12, 0x03, 'N', 0, 'S', 0, 'D', 0, 'N', 0, '-', 0, 'U', 0, 'S', 0, 'B', 0 };
const uint8_c usbJoyStrDesc[] = { 0x18, 0x03, 'N', 0, 'S', 0, 'D', 0, 'N', 0, ' ', 0, 'N', 0, 'T', 0, 'P', 0, ' ', 0, 'I', 0, 'I', 0 };
// START at 0x0000
uint8_x __at (0x0000) Ep0Buffer[THIS_ENDP0_SIZE];                                           //端点0 OUT&IN缓冲区，必须是偶地址
uint8_x __at (0x0008) Ep1Buffer[2 * MAX_PACKET_SIZE];                                       //端点1 OUT&IN缓冲区,必须是偶地址
uint8_x __at (0x0088) Ep3Buffer[2 * MAX_PACKET_SIZE];                                       //端点3 OUT&IN缓冲区,必须是偶地址
// END at 0x0108
/*
    手柄输入报表（11字节，无 Report ID），由 g_joy 打包：
        byte 0..1:  按键 1..15（小端，byte1 只用 bit0..bit6，bit7 固定 0）
        byte 2:     低 4 位 = hat（0..7，>7 视为居中），高 4 位固定 0（描述符里是常量填充）
        byte 3..4:  X    byte 5..6:  Y    byte 7..8:  Z    byte 9..10: Rz
                    （各为 16 位字段、小端补码，-1023..+1023，中点 0）
    原实现把 16 字节键盘缓冲同时当手柄报表用，byte0 兼职 Report ID，导致
    "松开全部按键"会把 Report ID 清成 0 而永远发不出去；这里彻底去掉 Report ID。
*/
uint8_x JoyReport[JOY_REPORT_SIZE] = { 0 };
uint8_x JoyLastReport[JOY_REPORT_SIZE] = { 0 };                                             //上一次真正发出去的帧（用于"内容没变就不发"）
__bit USB_OK = false;                                                                       //主机已完成 SET_CONFIGURATION
volatile __bit JoyDirty = true;                                                             //=1 时 joySendIfChanged() 必须发，即使内容没变
__bit RemoteWakeup = false;                                                                 //主机使能了远程唤醒（SET_FEATURE 记录，GET_STATUS 回读）
uint8_x HIDInput[HID_BUF_SIZE] = { 0 };                                                     //自定义HID接收缓冲
uint8_x HIDOutput[HID_BUF_SIZE] = { 0 };                                                    //自定义HID发送缓冲
uint8_t             SetupReqCode, SetupReqType, SetupLen, UsbConfig;
uint8_t*            pDescr;                                                                 //当前要上传的描述符指针
volatile __bit      HIDIN = 0;
volatile __bit      EP1Busy = 0;                                                            //手柄IN包已装好但主机还没取走
#define UsbSetupBuf ((PUSB_SETUP_REQ)Ep0Buffer)

void __usb_interrupt() __interrupt (INT_NO_USB) {
    uint8_t len;
	if (UIF_TRANSFER) {                                                                     // USB传输完成
		switch (USB_INT_ST & (MASK_UIS_TOKEN | MASK_UIS_ENDP)) {                            // 分析操作令牌和端点号
            case UIS_TOKEN_OUT | 3:                                                     // endpoint 3# 中断端点下传
                if (U_TOG_OK) {                                                         // 不同步的数据包将丢弃
                    len = USB_RX_LEN;
                    if (Ep3Buffer[0] == 0x55 && (len - 1) <= sizeof(HIDInput) && !HIDIN) {
                        memset(HIDInput, 0x00, sizeof(HIDInput));
                        memcpy(HIDInput, Ep3Buffer + 1, len - 1);
                        HIDIN = 1;
                    }
                }
                break;
                /* 原来这里处理 EP1 OUT（键盘 LED 输出报表），并顺手把 USB_OK 置真。
                   改成纯手柄后接口0没有 OUT 端点、也没有输出报表，谁都不能再从
                   数据阶段设置"主机已就绪"——那个状态只由标准 SET_CONFIGURATION
                   决定（见下面 USB_SET_CONFIGURATION 分支）。 */
            case UIS_TOKEN_IN | 3:                                                      //endpoint 3# 中断端点上传
                UEP3_T_LEN = 0;                                                         //预使用发送长度一定要清空
                UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;               //默认应答NAK
                break;
            case UIS_TOKEN_IN | 1:                                                      //endpoint 1# 中断端点上传
                UEP1_T_LEN = 0;                                                         //预使用发送长度一定要清空
                UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_NAK;               //默认应答NAK
                EP1Busy = 0;                                                            /*主机已取走，可以装下一包*/
                break;
            case UIS_TOKEN_SETUP | 0:                                                   // endpoint 0# SETUP
                len = USB_RX_LEN;
                if (len == sizeof(USB_SETUP_REQ)) {                                     // SETUP包长度
                    SetupLen = UsbSetupBuf->wLengthL;
                    if (UsbSetupBuf->wLengthH || SetupLen > 0x7F) SetupLen = 0x7F;      // 限制总长度
                    len = 0;                                                            // 默认为成功并且上传0长度
                    SetupReqCode = UsbSetupBuf->bRequest;
                    /* 必须在这里存下来：Ep0Buffer 会被 GET_DESCRIPTOR 的 memcpy 覆盖，
                       之后 UsbSetupBuf->bRequestType 读到的是描述符字节。状态阶段的
                       判断依赖这个值。 */
                    SetupReqType = UsbSetupBuf->bRequestType;
                    if ((UsbSetupBuf->bRequestType & USB_REQ_TYP_MASK) != USB_REQ_TYP_STANDARD) { /* 非标准请求 */
                        switch (SetupReqCode) {
                            case 0x01://GetReport
                                break;
                            case 0x02://GetIdle
                                break;
                            case 0x03://GetProtocol
                                break;
                            case 0x09://SetReport
                                break;
                            case 0x0A://SetIdle
                                break;
                            case 0x0B://SetProtocol
                                break;
                            default:
                                len = 0xFF;  								            /*命令不支持*/
                                break;
                        }
                    } else {                                                            // 标准请求
                        switch (SetupReqCode) {                                         // 请求码
                            case USB_GET_DESCRIPTOR:
                                switch (UsbSetupBuf->wValueH) {
                                    case 1:                                             // 设备描述符
                                        pDescr = (uint8_t*) (&usbDevDesc[0]);
                                        len = sizeof(usbDevDesc);
                                        break;
                                    case 2:                                             // 配置描述符
                                        pDescr = (uint8_t*) (&usbCfgDesc[0]);
                                        len = sizeof(usbCfgDesc);
                                        break;
                                    case 3:                                             // 字符串描述符
                                        switch(UsbSetupBuf->wValueL) {
                                            case 0:
                                                pDescr = (uint8_t*) (&usbLangDesc[0]);
                                                len = sizeof(usbLangDesc);
                                                break;
                                            case 1:
                                                pDescr = (uint8_t*) (&usbManuDesc[0]);
                                                len = sizeof(usbManuDesc);
                                                break;
                                            case 2:
                                                pDescr = (uint8_t*) (&usbProdDesc[0]);
                                                len = sizeof(usbProdDesc);
                                                break;
                                            case 3:
                                                pDescr = (uint8_t*) (&usbSerialDesc[0]);
                                                len = sizeof(usbSerialDesc);
                                                break;
                                            case 4:
                                                pDescr = (uint8_t*) (&usbCfgStrDesc[0]);
                                                len = sizeof(usbCfgStrDesc);
                                                break;
                                            case 5:
                                                pDescr = (uint8_t*) (&usbJoyStrDesc[0]);
                                                len = sizeof(usbJoyStrDesc);
                                                break;
                                            default:
                                                len = 0xFF;                             // 不支持的字符串描述符
                                                break;
                                        }
                                        break;
                                    case 0x22:                                          //报表描述符
                                        if (UsbSetupBuf->wIndexL == 0) {                //接口0报表描述符
                                            pDescr = (uint8_t*) (&JoyRepDesc[0]);       //数据准备上传
                                            len = sizeof(JoyRepDesc);
                                        } else if (UsbSetupBuf->wIndexL == 1) {         //接口1报表描述符
                                            pDescr = (uint8_t*) (&CustomRepDesc[0]);    //数据准备上传
                                            len = sizeof(CustomRepDesc);
                                        } else {
                                            len = 0xff;                                 //本程序只有2个接口，这句话正常不可能执行
                                        }
                                        break;
                                    default:
                                        len = 0xFF;                                     // 不支持的描述符类型
                                        break;
                                }
                                /* 修复：原代码在这里无条件往下执行拷贝，把上面 default 分支
                                   设的 len=0xFF 错误标记覆盖成一个正常的 0..8 长度，于是
                                   “if (len == 0xFF) STALL” 永远不成立：未知描述符类型
                                   （如 GET_DESCRIPTOR(0x21)）和未知字符串索引不会返回
                                   STALL，而是从没赋过值的 pDescr 拷 8 字节垃圾发给主机。
                                   先在这里跳出，让它落到统一的 STALL 分支。
                                   （假定没有哪个描述符正好 255 字节——本项目最大 79。） */
                                if (len == 0xFF) break;
                                if (SetupLen > len) SetupLen = len;                     // 限制总长度
                                len = SetupLen >= THIS_ENDP0_SIZE ? THIS_ENDP0_SIZE : SetupLen;  // 本次传输长度
                                memcpy(Ep0Buffer, pDescr, len);                         /* 加载上传数据 */
                                SetupLen -= len;
                                pDescr += len;
                                break;
                            case USB_SET_ADDRESS:
                                SetupLen = UsbSetupBuf->wValueL;                        // 暂存USB设备地址
                                break;
                            case USB_GET_CONFIGURATION:
                                Ep0Buffer[0] = UsbConfig;
                                if (SetupLen >= 1) len = 1;
                                break;
                            case USB_SET_CONFIGURATION:
                                UsbConfig = UsbSetupBuf->wValueL;
                                /* 修复：以前“主机已就绪”的 USB_OK 挂在键盘 LED 输出
                                   报表上（见已删除的 EP1 OUT 分支），手柄根本没有输出
                                   报表，那样永远等不到；而且 USB_OK 从不清零，拔插后
                                   会残留。现在只由标准 SET_CONFIGURATION 决定。 */
                                USB_OK = (UsbConfig != 0);
                                /* (重新)配置时把端点握手恢复成已知状态：清掉上一次会话
                                   可能残留的 STALL，并按规范让翻转位从 DATA0 重新开始
                                   （bUEP_AUTO_TOG 置上，之后由硬件接管翻转）。 */
                                UEP1_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
                                UEP3_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
                                UEP1_T_LEN = 0;
                                UEP3_T_LEN = 0;
                                EP1Busy = 0;
                                JoyDirty = true;                /* 配置后的第一帧必须发出去，哪怕内容没变 */
                                break;
                            case USB_CLEAR_FEATURE:
                                /* 修复：原代码在端点分支里做完握手复位后，又用一个
                                   “接收者不是设备就 len=0xFF” 的 else 把自己盖掉，
                                   于是 CLEAR_FEATURE(ENDPOINT_HALT) **永远**返回
                                   STALL——端点被 SET_FEATURE halt 之后，主机再也
                                   没有标准办法把它解挂。这里改成按接收者分派，只对
                                   真正不支持的组合 STALL。 */
                                if ((UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                                    /* 端点特性只有 ENDPOINT_HALT(0) 可以清 */
                                    if ((((uint16_t) UsbSetupBuf->wValueH << 8) | UsbSetupBuf->wValueL) != 0x00) {
                                        len = 0xFF;
                                        break;
                                    }
                                    switch (UsbSetupBuf->wIndexL) {
                                        case 0x81:
                                            UEP1_CTRL = UEP1_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                            break;
                                        case 0x83:
                                            UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_T_TOG | MASK_UEP_T_RES ) | UEP_T_RES_NAK;
                                            break;
                                        case 0x03:
                                            UEP3_CTRL = UEP3_CTRL & ~ ( bUEP_R_TOG | MASK_UEP_R_RES ) | UEP_R_RES_ACK;
                                            break;
                                        default:
                                            len = 0xFF;                     // 不存在的端点（EP1 OUT 已随键盘接口删除）
                                            break;
                                    }
                                } else if ((UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE) {
                                    /* 设备特性只有 DEVICE_REMOTE_WAKEUP(1) 可以清 */
                                    if ((((uint16_t) UsbSetupBuf->wValueH << 8) | UsbSetupBuf->wValueL) == 0x01)
                                        RemoteWakeup = false;
                                    else
                                        len = 0xFF;
                                } else {
                                    len = 0xFF;                             // 接口/其它接收者没有可清的特性
                                }
                                break;
                            case USB_SET_FEATURE:                                                               /* Set Feature */
                                if ((UsbSetupBuf->bRequestType & 0x1F) == 0x00) {                               /* 设置设备 */
                                    if ((((uint16_t) UsbSetupBuf->wValueH << 8) | UsbSetupBuf->wValueL) == 0x01)  {
                                        if (usbCfgDesc[7] & 0x20) {
                                            /* 修复：原来这里只有一句空注释，状态没存下来，
                                               GET_STATUS 永远回 “不支持远程唤醒”，和已经
                                               接受了 SET_FEATURE 的事实矛盾。 */
                                            RemoteWakeup = true;
                                        } else {
                                            len = 0xFF;                                                         /* 操作失败 */
                                        }
                                    } else {
                                        len = 0xFF;                                                             /* 操作失败 */
                                    }
                                } else if ((UsbSetupBuf->bRequestType & 0x1F) == 0x02) {                        /* 设置端点 */
                                    if ((((uint16_t) UsbSetupBuf->wValueH << 8) | UsbSetupBuf->wValueL ) == 0x00) {
                                        switch (((uint16_t) UsbSetupBuf->wIndexH << 8 ) | UsbSetupBuf->wIndexL) {
                                        case 0x81:
                                            UEP1_CTRL = UEP1_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;            /* 设置端点1 IN STALL */
                                            break;
                                        case 0x83:
                                            UEP3_CTRL = UEP3_CTRL & (~bUEP_T_TOG) | UEP_T_RES_STALL;            /* 设置端点3 IN STALL */
                                            break;
                                        case 0x03:
                                            UEP3_CTRL = UEP3_CTRL & (~bUEP_R_TOG) | UEP_R_RES_STALL;            /* 设置端点3 OUT STALL */
                                            break;
                                        default:
                                            len = 0xFF;                                                         //操作失败
                                            break;
                                        }
                                    } else {
                                        len = 0xFF;                                                             //操作失败
                                    }
                                } else {
                                    len = 0xFF;                                                                 //操作失败
                                }
                                break;
                            case USB_GET_INTERFACE:
                                /* 两个接口都只有 alternate setting 0 */
                                Ep0Buffer[0] = 0x00;
                                if (SetupLen >= 1) len = 1;
                                break;
                            case USB_SET_INTERFACE:
                                /* 原来没有这个分支，主机的合法 SET_INTERFACE(0, alt=0)
                                   会掉进 default 被当成未知请求 STALL。 */
                                if (UsbSetupBuf->wValueL != 0 || UsbSetupBuf->wIndexL > 1)
                                    len = 0xFF;                             // 只有接口0/1的 alt 0 存在
                                break;
                            case USB_GET_STATUS:
                                /* 修复：原来无条件回 0x0000，被 halt 的端点被报成“正常”，
                                   远程唤醒使能也永远回 0，和 SET_FEATURE/CLEAR_FEATURE
                                   实际维护的状态矛盾。 */
                                Ep0Buffer[0] = 0x00;
                                Ep0Buffer[1] = 0x00;
                                if ((UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_DEVICE) {
                                    if (RemoteWakeup) Ep0Buffer[0] |= 0x02;          // bit1: 远程唤醒使能
                                } else if ((UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                                    switch (UsbSetupBuf->wIndexL) {
                                        case 0x81: if ((UEP1_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL) Ep0Buffer[0] |= 0x01; break;
                                        case 0x83: if ((UEP3_CTRL & MASK_UEP_T_RES) == UEP_T_RES_STALL) Ep0Buffer[0] |= 0x01; break;
                                        case 0x03: if ((UEP3_CTRL & MASK_UEP_R_RES) == UEP_R_RES_STALL) Ep0Buffer[0] |= 0x01; break;
                                        default:   len = 0xFF;                           // 不存在的端点
                                                   break;
                                    }
                                    if (len == 0xFF) break;
                                } else if ((UsbSetupBuf->bRequestType & USB_REQ_RECIP_MASK) != USB_REQ_RECIP_INTERF) {
                                    len = 0xFF;                                     // 不支持的接收者
                                    break;
                                }
                                if (SetupLen >= 2) len = 2;
                                else len = SetupLen;
                                break;
                            default:
                                len = 0xFF;                                                                     // 操作失败
                                break;
                        }
                    }
                } else {
                    len = 0xFF;                                                                                 // SETUP包长度错误
                }
                if (len == 0xFF) {                                                                              // 操作失败
                    SetupReqCode = 0xFF;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;                    // STALL
                } else if (len <= THIS_ENDP0_SIZE) {                                                            // 上传数据或者状态阶段返回0长度包
                    UEP0_T_LEN = len;
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;                        // 默认数据包是DATA1
                } else {                                                                                        // 下传数据或其它
                    UEP0_T_LEN = 0;                                                                             // 虽然尚未到状态阶段，但是提前预置上传0长度数据包以防主机提前进入状态阶段
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;                        // 默认数据包是DATA1
                }
                break;
            case UIS_TOKEN_IN | 0:                                                                              // endpoint 0# IN
                switch (SetupReqCode) {
                    case USB_GET_DESCRIPTOR:
                        len = SetupLen >= THIS_ENDP0_SIZE ? THIS_ENDP0_SIZE : SetupLen;                         // 本次传输长度
                        memcpy(Ep0Buffer, pDescr, len);                                                         /* 加载上传数据 */
                        SetupLen -= len;
                        pDescr += len;
                        UEP0_T_LEN = len;
                        UEP0_CTRL ^= bUEP_T_TOG;                                                                // 翻转
                        break;
                    case USB_SET_ADDRESS:
                        USB_DEV_AD = USB_DEV_AD & bUDA_GP_BIT | SetupLen;
                        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        break;
                    default:
                        UEP0_T_LEN = 0;                                                                         // 状态阶段完成中断或者是强制上传0长度数据包结束控制传输
                        UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        break;
                }
                break;
            case UIS_TOKEN_OUT | 0:                                                                             // endpoint 0# OUT（控制传输的数据阶段）
                /* 请求码 0x09 有两种含义：标准 SET_CONFIGURATION 和类 SET_REPORT，
                   只看 SetupReqCode 分不清。EP0 上的 OUT 令牌有两种来源：
                     (1) 控制读（bmRequestType 方向位=1）的**状态阶段** —— 必须
                         ACK。所有 GET_* 请求都要走这里，STALL 掉整个控制传输就失败。
                     (2) 标准控制写的 OUT 数据阶段 —— 本设备不支持，STALL。
                   判据必须用 SETUP 时存下的 SetupReqType：Ep0Buffer[0] 早在
                   GET_DESCRIPTOR 的 memcpy 里被描述符数据覆盖，直接读
                   UsbSetupBuf->bRequestType 等于拿描述符字节做判断 —— 配置
                   描述符最后一包留下 0x00 被判成"标准控制写"，控制读被 STALL，
                   表现为"配置描述符头部 9 字节读得到、完整 66 字节读不到"。
                   两种情况都绝不能在这里设置 USB_OK ——"主机已就绪"只能来自
                   SET_CONFIGURATION。 */
                if ((SetupReqType & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD &&
                    (SetupReqType & USB_REQ_TYP_IN) == 0) {
                    UEP0_CTRL = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
                } else {
                    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;                                              // 准备下一控制传输
                    UEP0_CTRL ^= bUEP_R_TOG;                                                                //同步标志位翻转
                }
                break;
            default:
                break;
        }
		UIF_TRANSFER = 0;                                                                                           // 清中断标志
	} else if (UIF_BUS_RST) {                                                                                       // USB总线复位
		UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
		UEP1_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
        UEP3_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;
		USB_DEV_AD = 0x00;
		/* 修复：复位后设备回到 Default 状态，原代码却把 UsbConfig/USB_OK 留着旧值，
		   主机会读到一个“已配置”但地址为 0 的假象；远程唤醒使能同样必须清。
		   顺便清掉 EP1 的“上一包未取走”标志，免得复位后白发一次超时。 */
		UsbConfig = 0;
		USB_OK = false;
		RemoteWakeup = false;
		EP1Busy = 0;
		UEP1_T_LEN = 0;
		UEP3_T_LEN = 0;
		JoyDirty = true;
		UIF_SUSPEND = 0;
		UIF_TRANSFER = 0;
		UIF_BUS_RST = 0;                                                                                            // 清中断标志
	} else if (UIF_SUSPEND) {                                                                                       // USB总线挂起/唤醒完成
		UIF_SUSPEND = 0;
		if (USB_MIS_ST & bUMS_SUSPEND) {                                                                          // 挂起
			while (XBUS_AUX & bUART0_TX);                                                                         // 等待发送完成
			SAFE_MOD = 0x55;
			SAFE_MOD = 0xAA;
			WAKE_CTRL = bWAK_BY_USB | bWAK_RXD0_LO;                                                                 // USB或者RXD0有信号时可被唤醒
			PCON |= PD;                                                                                             // 睡眠
			SAFE_MOD = 0x55;
			SAFE_MOD = 0xAA;
			WAKE_CTRL = 0x00;
		} else {                                                                                                    // 唤醒

		}
	} else {                                                                                                        // 意外的中断,不可能发生的情况
		USB_INT_FG = 0xFF;                                                                                          // 清中断标志
	}
}

void usbDevInit() {
	IE_USB = 0;
	USB_CTRL = 0x00;                                                           // 先设定USB设备模式
    UDEV_CTRL = bUD_DP_PD_DIS | bUD_DM_PD_DIS;                                 // 禁止DP/DM下拉电阻

    SET_UEP_DMA_ADDR(UEP0, Ep0Buffer);                                          //端点0数据传输地址
    UEP4_1_MOD &= ~(bUEP4_RX_EN | bUEP4_TX_EN);                                 //端点0单64字节收发缓冲区
    UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;                                  //OUT事务返回ACK，IN事务返回NAK
    SET_UEP_DMA_ADDR(UEP1, Ep1Buffer);                                          //端点1数据传输地址
    /* 端点1仍按 1 1 0 模式配成 64B OUT + 64B IN：接口0虽然只剩 IN 端点，
       但保持这个模式才能让 IN 缓冲区留在 Ep1Buffer+MAX_PACKET_SIZE 处
       （见 Enp1IntIn），改动这里必须同步改那个偏移。 */
    UEP4_1_MOD = UEP4_1_MOD & ~bUEP1_BUF_MOD | bUEP1_RX_EN | bUEP1_TX_EN;       //端点1收发使能 128字节缓冲区
    UEP1_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;                  //OUT事务返回ACK，IN事务返回NAK
    SET_UEP_DMA_ADDR(UEP3, Ep3Buffer);                                          //端点3数据传输地址
    UEP2_3_MOD = UEP2_3_MOD & ~bUEP3_BUF_MOD | bUEP3_RX_EN | bUEP3_TX_EN;       //端点3收发使能 128字节缓冲区
    UEP3_CTRL = bUEP_AUTO_TOG | UEP_R_RES_ACK | UEP_T_RES_NAK;                  //OUT事务返回ACK，IN事务返回NAK

	USB_DEV_AD = 0x00;
	USB_CTRL |= bUC_DEV_PU_EN | bUC_INT_BUSY | bUC_DMA_EN;                      // 启动USB设备及DMA，在中断期间中断标志未清除前自动返回NAK
	USB_CTRL |= bUC_SYS_CTRL1;
    UDEV_CTRL |= bUD_PORT_EN;                                                   // 允许USB端口
	USB_INT_FG = 0xFF;                                                          // 清中断标志
	USB_INT_EN = bUIE_SUSPEND | bUIE_TRANSFER | bUIE_BUS_RST;
	IE_USB = 1;

    UEP1_T_LEN = 0;
    UEP3_T_LEN = 0;
}

void Enp1IntIn(uint8_t* addr, uint8_t len) {
    uint16_t t;
    if (!USB_OK) return;                                                        //主机还没配置好，发出去也没人收
    if (len == 0 || len > EP1_IN_MAX_PACKET) return;                            //长度非法：宁可丢这一帧也不能越界/超包
    /* 修复：原来这里是 while (FLAG == 0);，而 FLAG 只在 usbDevInit() 和
       EP1 IN 完成中断里被置 1、从来没清过，所以那句等待形同虚设（挡不住
       覆盖正在发送的包）；如果真按原意清零，主机停止轮询时又会永久卡死
       主循环。这里改成“有界等待”：最多自旋 0xFFFF 次，超时就覆盖旧数据，
       丢一帧总比死机好。 */
    for (t = 0; t < 0xFFFF && EP1Busy; t++);
    memcpy(Ep1Buffer + MAX_PACKET_SIZE, addr, len);                             //加载上传数据
    UEP1_T_LEN = len;                                                           //上传数据长度
    EP1Busy = 1;                                                                //装好了，等 IN 完成中断清零
    UEP1_CTRL = UEP1_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;                   //有数据时上传数据并应答ACK
}

void Enp3IntIn() {
    Ep3Buffer[MAX_PACKET_SIZE] = 0xAA;
    memcpy(Ep3Buffer + MAX_PACKET_SIZE + 1, HIDOutput, sizeof(HIDOutput));      //加载上传数据
    UEP3_T_LEN = sizeof(HIDOutput) + 1;                                         //上传数据长度
    UEP3_CTRL = UEP3_CTRL & ~ MASK_UEP_T_RES | UEP_T_RES_ACK;                   //有数据时上传数据并应答ACK
}

/* ---------------------------------------------------------------- 手柄输入上报
 * 只有这一处把 g_joy 打包成 11 字节载荷。业务层只写 g_joy（见 joy.h）。
 * 轴字段是有符号的，直接按补码取低/高字节即可（负数不用特殊处理）。 */
static void joyPack(void) {
    JoyReport[0]  = (uint8_t) (g_joy.buttons & 0x00FF);         //按键 1..8
    JoyReport[1]  = (uint8_t) ((g_joy.buttons >> 8) & 0x007F);  //按键 9..15，bit15 保留为 0
    JoyReport[2]  = (uint8_t) (g_joy.hat & 0x0F);               //hat 在低 4 位，高 4 位是描述符里的常量填充
    JoyReport[3]  = (uint8_t) (((uint16_t) g_joy.x) & 0x00FF);  //X 小端补码
    JoyReport[4]  = (uint8_t) (((uint16_t) g_joy.x) >> 8);
    JoyReport[5]  = (uint8_t) (((uint16_t) g_joy.y) & 0x00FF);  //Y
    JoyReport[6]  = (uint8_t) (((uint16_t) g_joy.y) >> 8);
    JoyReport[7]  = (uint8_t) (((uint16_t) g_joy.z) & 0x00FF);  //Z
    JoyReport[8]  = (uint8_t) (((uint16_t) g_joy.z) >> 8);
    JoyReport[9]  = (uint8_t) (((uint16_t) g_joy.rz) & 0x00FF); //Rz
    JoyReport[10] = (uint8_t) (((uint16_t) g_joy.rz) >> 8);
}

/* 返回 1 表示当前 JoyReport 和上一次发出去的一模一样 */
static uint8_t joySameAsLastSent(void) {
    uint8_t i;
    for (i = 0; i < JOY_REPORT_SIZE; i++)
        if (JoyReport[i] != JoyLastReport[i]) return 0;
    return 1;
}

__bit joyReady(void) {
    return USB_OK;
}

void joyInvalidate(void) {
    JoyDirty = true;
}

void joySend(void) {
    if (!USB_OK) return;                                        //joy.h 约定：未配置时是无害空操作
    joyPack();
    Enp1IntIn(JoyReport, JOY_REPORT_SIZE);
    memcpy(JoyLastReport, JoyReport, JOY_REPORT_SIZE);
    JoyDirty = false;
}

uint8_t joySendIfChanged(void) {
    if (!USB_OK) return 0;                                      //未配置：没发，返回 0
    joyPack();
    /* JoyDirty 由 joyInvalidate()/SET_CONFIGURATION/总线复位置位，
       所以“全 0 载荷”和“和上一帧一模一样但刚被要求重发”两种情况都能发出去
       ——这正是原来 if (type > 0) 把松键帧吞掉的 bug。 */
    if (!JoyDirty && joySameAsLastSent()) return 0;
    Enp1IntIn(JoyReport, JOY_REPORT_SIZE);
    memcpy(JoyLastReport, JoyReport, JOY_REPORT_SIZE);
    JoyDirty = false;
    return 1;
}

/* ---------------------------------------------------------------- 厂商自定义接口
 * 接口1（Report ID 0xAA 输入 / 0x55 输出）原样保留，用于调试文本和主机下发。 */
__bit joyHasHostData(void) {
    return HIDIN;
}

uint8_t joyHostData(uint8_t index) {
    return HIDInput[index % sizeof(HIDInput)];
}

void joyRequestHostData(void) {
    HIDIN = 0;
}

void joyPrint(const char* str) {
    uint8_t i = 0;
    memset(HIDOutput, 0x00, sizeof(HIDOutput));
    /* 按 joy.h 的约定截断到 HID_BUF_SIZE-1 字节。原实现是无长度检查的
       strcpy，调用方给长了就会写穿 HIDOutput（xdata 里的相邻变量）。 */
    while (i < (HID_BUF_SIZE - 1) && str[i] != '\0') {
        HIDOutput[i] = (uint8_t) str[i];
        i++;
    }
    Enp3IntIn();
}

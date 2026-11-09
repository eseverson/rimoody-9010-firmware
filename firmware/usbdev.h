/* U-Boot 2014.04 struct usb_device, as built into this unit's U-Boot: the fields our code reads and writes. */
#ifndef USBDEV_H
#define USBDEV_H
/* struct usb_device field offsets in this build (from usb_new_device / usb_maxpacket / ehci_submit_async) */
#define UDEV_DEVNUM    0x000
#define UDEV_SPEED     0x008   /* 3 = high speed */
#define UDEV_MAXPACKET 0x06c   /* encoded PACKET_SIZE_*, goes into the pipe's low bits */
#define UDEV_TOGGLE    0x070   /* toggle[0] IN, toggle[1] OUT, one bit per endpoint */
#define UDEV_EPMAX_IN  0x080   /* int epmaxpacketin[16] (EHCI takes the QH max packet from here) */
#define UDEV_EPMAX_OUT 0x0c0   /* int epmaxpacketout[16] */
#define UDEV_DESC      0x140   /* struct usb_device_descriptor; idVendor at +8 */
#define UDEV_STATUS    0x954   /* 0 ok, 2 stall, 4 buf err, 8 babble, 0x20 crc/xact (|2 halted), 0x80000000 timeout */
#define UDEV_ACT_LEN   0x958
/* U-Boot USB_ST_* values in UDEV_STATUS */
#define USB_ST_STALLED    0x02
#define USB_ST_BUF_ERR    0x04
#define USB_ST_BABBLE_DET 0x08
#define USB_ST_CRC_ERR    0x20
#define USB_ST_NOT_PROC   0x80000000u
#endif

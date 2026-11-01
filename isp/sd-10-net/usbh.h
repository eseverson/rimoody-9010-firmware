/* Our own `usb start` for the Wi-Fi bus (usbh.c). Net task only. */
#ifndef USBH_H
#define USBH_H
#define USBH_DEVNUM 2
struct usbh_info { unsigned int step, portsc, otp, n_eps; };   /* step: 100 = done, else the step that failed */
extern struct usbh_info usbh_info;
void *usbh_start(void);   /* the enumerated device (struct usb_device layout, usbdev.h), or 0 */
#endif

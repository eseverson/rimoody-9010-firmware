/* Our own EHCI transfers on the Wi-Fi dongle's controller (ehci.c). Net task only. Same calling convention and
 * return values as U-Boot 2014.04 usb_control_msg / usb_bulk_msg; status and act_len land in the struct usb_device. */
#ifndef EHCI_H
#define EHCI_H
int ehci_ctrl(void *dev, unsigned int pipe, unsigned char req, unsigned char reqtype, unsigned short value,
              unsigned short index, void *data, unsigned short size, int timeout_ms);   /* bytes, or -1 */
int ehci_bulk(void *dev, unsigned int pipe, void *data, int len, int *actual, int timeout_ms);   /* 0 or -1 */
/* v2: a bulk IN kept queued. 0 = data (*actual bytes in data), -2 = nothing yet (still queued), -1 = error or
 * refused (data must be cache-line aligned in address and length, at most one qTD). Call again to re-queue. */
int ehci_rx_poll(void *dev, unsigned int pipe, void *data, int len, int *actual);
void ehci_rx_stop(void);   /* before U-Boot uses USB, and before memory under the buffer is reused */
/* v4: a bulk OUT queued on the running ring without waiting for it (one OUT endpoint; data copied, <= 2048 bytes).
 * 0 = queued, -1 = not possible now (the RX ring is not up, or another endpoint): send it with ehci_bulk instead,
 * -2 = dropped (ring stuck or the endpoint halted). Once an endpoint is queued here, all its frames must come here
 * until a -1: its data toggle lives in the queue head. */
int ehci_tx_async(void *dev, unsigned int pipe, const void *data, int len);
unsigned int ehci_portsc(void);
struct ehci_stats { unsigned int ok, errors, timeouts, busy, refused, rx, txq, txerr, txfull; };
extern struct ehci_stats ehci_stats;
#endif

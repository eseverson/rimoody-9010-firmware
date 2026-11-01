/* Our own `usb start` for the Wi-Fi bus (notes/usb_host.md): USB PHY and host-mode init as U-Boot's ehci_hcd_init
 * (0xf7f8f2: uphy_init 0xf7f5c2 + usb_power_init 0xf7f86a), EHCI1 reset and start as 0xf7ecfe, root-port reset, and
 * enumeration of the one high-speed device into a struct laid out like U-Boot's struct usb_device (usbdev.h), so
 * net.c and ehci.c keep using it unchanged. Transfers go through ehci.c. Net task only. */
#include "usbh.h"
#include "ehci.h"
#include "usbdev.h"
#include "clk.h"

typedef unsigned int u32;
typedef unsigned char u8;
#define REG(a) (*(volatile u32 *)(a))
#define F32(p) (*(u32 *)(p))

#define HCCR     0x9c104100u
#define HCOR     0x9c104120u
#define USBCMD   (HCOR + 0x00)
#define USBSTS   (HCOR + 0x04)
#define USBINTR  (HCOR + 0x08)
#define CONFIGFLAG (HCOR + 0x40)
#define PORTSC   (HCOR + 0x44)
#define PS_CCS   (1u << 0)
#define PS_PE    (1u << 1)
#define PS_W1C   (0x2au)            /* CSC, PEC, OCC */
#define PS_PR    (1u << 8)
#define PS_LS    (3u << 10)
#define PS_OWNER (1u << 13)

struct usbh_info usbh_info;
static u8 udev[0x9c0] __attribute__((aligned(64)));   /* U-Boot's sizeof(struct usb_device) in this build */
static u8 dbuf[512] __attribute__((aligned(64)));

static void ms(u32 n) { clk_udelay(n * 1000u); }

static u32 disc(u32 c)   /* OTP disconnect-level field -> PHY setting, as 0xf7f784 (signed compare) */
{
    return (int)c - 1 < 30 ? (c < 12 ? c + 2 : c) : 13;
}

static void uphy_init(void)
{
    REG(0x9c00005c) = 0x03000300;   /* masked write, high half = mask: PHY resets on, then off */
    ms(1);
    REG(0x9c00005c) = 0x03000000;
    ms(1);
    REG(0x9c004af0) = 0x87474002;
    REG(0x9c004b70) = 0x87474004;
    static const u32 seq[] = { 0x88, 0x80, 0x88, 0x80 };
    for (int i = 0; i < 4; i++) {
        REG(0x9c004af8) = seq[i];
        REG(0x9c004b78) = seq[i];
        ms(1);
    }
    REG(0x9c004af8) = 0;
    REG(0x9c004b78) = 0;
    ms(20);
    REG(0x9c00005c) = 0x00c000c0;
    ms(1);
    REG(0x9c00005c) = 0x00c00000;
    ms(1);
    REG(0x9c004acc) |= 0xf;
    REG(0x9c004b4c) |= 0xf;
    u32 otp = REG(0x9c00af18);
    REG(0x9c004a9c) = (REG(0x9c004a9c) & ~0x1fu) | disc(otp & 0x1f);
    REG(0x9c004b1c) = (REG(0x9c004b1c) & ~0x1fu) | disc((otp >> 8) & 0x1f);
    u32 inv = REG(0x9c00af08);
    if (inv & 1)
        REG(0x9c004af4) |= 0x40;
    if (inv & 2)
        REG(0x9c004b74) |= 0x40;
    usbh_info.otp = otp;
}

static int wait(u32 reg, u32 mask, u32 want, u32 ms_max)
{
    u32 t0 = clk_ms();
    while ((REG(reg) & mask) != want)
        if (clk_ms() - t0 > ms_max)
            return -1;
    return 0;
}

static u32 pipe(int in, int devnum) { return 2u << 30 | (u32)devnum << 8 | 3u | (in ? 0x80u : 0); }   /* ctrl, ep 0 */

static int get_desc(int devnum, int type, void *buf, int len)
{
    return ehci_ctrl(udev, pipe(1, devnum), 6, 0x80, (unsigned short)(type << 8), 0, buf, (unsigned short)len, 1000);
}

#define FAIL(n) do { usbh_info.step = n; usbh_info.portsc = REG(PORTSC); ehci_rx_stop(); return 0; } while (0)

void *usbh_start(void)
{
    usbh_info = (struct usbh_info){ 0 };
    ehci_rx_stop();
    uphy_init();
    REG(0x9c00018c) = 0x70007;   /* usb_power_init(1): "switch to host" */

    REG(USBCMD) = REG(USBCMD) & ~1u;   /* stop, then HCRESET */
    wait(USBSTS, 1u << 12, 1u << 12, 20);
    REG(USBCMD) = REG(USBCMD) | 2u;
    if (wait(USBCMD, 2u, 0, 250))
        FAIL(1);
    REG(USBINTR) = 0;
    REG(USBCMD) = (REG(USBCMD) & ~0xb2u) | 1u;   /* run; no periodic / async schedule yet */
    if (wait(USBSTS, 1u << 12, 0, 20))
        FAIL(2);
    REG(CONFIGFLAG) = 1;   /* ports to EHCI, not the companion */
    ms(5);

    if (wait(PORTSC, PS_CCS, PS_CCS, 1000))   /* HCSPARAMS 0x1101: no port power control, port always powered */
        FAIL(3);
    ms(100);   /* USB 2.0 7.1.7.3: debounce before reset */
    u32 ps = REG(PORTSC);
    if ((ps & PS_LS) == (1u << 10))
        FAIL(4);   /* K state: low speed, belongs to the companion */
    REG(PORTSC) = (ps & ~(PS_PE | PS_W1C)) | PS_PR;
    ms(50);
    REG(PORTSC) = REG(PORTSC) & ~(PS_PR | PS_W1C);
    if (wait(PORTSC, PS_PR, 0, 10))
        FAIL(5);
    ms(2);
    ps = REG(PORTSC);
    usbh_info.portsc = ps;
    if (!(ps & PS_PE) || (ps & PS_OWNER))
        FAIL(6);   /* not high speed: the dongle is, so something is wrong */
    ms(10);   /* reset recovery */

    for (u32 i = 0; i < sizeof udev; i++)
        udev[i] = 0;
    F32(udev + UDEV_DEVNUM) = 0;
    F32(udev + UDEV_SPEED) = 3;
    F32(udev + UDEV_MAXPACKET) = 3;   /* PACKET_SIZE_64 */
    F32(udev + UDEV_EPMAX_IN) = 64;
    F32(udev + UDEV_EPMAX_OUT) = 64;
    if (get_desc(0, 1, dbuf, 8) != 8)
        FAIL(7);
    u32 mp0 = dbuf[7];
    if (mp0 != 64)
        FAIL(8);
    if (ehci_ctrl(udev, pipe(0, 0), 5, 0, USBH_DEVNUM, 0, 0, 0, 1000) < 0)   /* SET_ADDRESS */
        FAIL(9);
    ms(10);
    F32(udev + UDEV_DEVNUM) = USBH_DEVNUM;
    if (get_desc(USBH_DEVNUM, 1, dbuf, 18) != 18)
        FAIL(10);
    for (int i = 0; i < 18; i++)
        udev[UDEV_DESC + i] = dbuf[i];
    if (get_desc(USBH_DEVNUM, 2, dbuf, 9) != 9)
        FAIL(11);
    int total = dbuf[2] | dbuf[3] << 8;
    if (total > (int)sizeof dbuf)
        total = sizeof dbuf;
    int r = get_desc(USBH_DEVNUM, 2, dbuf, total);
    if (r < 9)
        FAIL(12);
    u32 cfgval = dbuf[5];
    for (int i = 0; i + 2 <= r && dbuf[i] >= 2 && i + dbuf[i] <= r; i += dbuf[i]) {   /* endpoints of interface 0 alt 0 */
        const u8 *d = dbuf + i;
        if (d[1] == 4 && d[0] >= 9 && (d[2] || d[3]))
            break;
        if (d[1] == 5 && d[0] >= 7) {
            u32 ep = d[2] & 0xf, mp = (d[4] | d[5] << 8) & 0x7ff;
            F32(udev + (d[2] & 0x80 ? UDEV_EPMAX_IN : UDEV_EPMAX_OUT) + 4 * ep) = mp;
            usbh_info.n_eps++;
        }
    }
    if (ehci_ctrl(udev, pipe(0, USBH_DEVNUM), 9, 0, (unsigned short)cfgval, 0, 0, 0, 1000) < 0)   /* SET_CONFIGURATION */
        FAIL(13);
    F32(udev + UDEV_TOGGLE) = 0;
    F32(udev + UDEV_TOGGLE + 4) = 0;
    usbh_info.step = 100;
    return udev;
}

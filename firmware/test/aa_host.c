/* Run aa.c + aa_tls.c against a real phone from a PC (libusb), before the head unit's phone port works:
 *   aa_host VID:PID|any CERT.pem KEY.pem [seconds] [video.h264]
 * Build (in test/): gcc -O1 -DAA_TLS_HOST -I.. -I../mbedtls/include '-DMBEDTLS_CONFIG_FILE="aa_tls_config.h"' -o aa_host aa_host.c ../aa.c ../aa_tls.c ../mbedtls/library/*.c -lusb-1.0
 * Switches the phone to Android accessory mode (AOA) if needed, then speaks the AA protocol, logging every state
 * change and saving the H.264 stream. The certificate and key are read from files at run time, never built in. */
#include "../aa.h"
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static libusb_device_handle *h;
static unsigned char ep_in, ep_out;
static FILE *vf;

static unsigned long long now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int send_cb(const unsigned char *p, int n)
{
    int done = 0;
    int r = libusb_bulk_transfer(h, ep_out, (unsigned char *)p, n, &done, 1000);
    if (r || done != n)
        fprintf(stderr, "bulk out: %s (%d of %d)\n", libusb_error_name(r), done, n);
    return r ? -1 : done;
}

static void video_cb(const unsigned char *p, int n, unsigned long long ts)
{
    (void)ts;
    if (vf)
        fwrite(p, 1, (size_t)n, vf);
}

static void log_cb(const char *s) { fputs(s, stdout); fflush(stdout); }

static unsigned char *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    unsigned char *b = malloc(16384);
    *n = fread(b, 1, 16383, f);
    b[(*n)++] = 0;
    fclose(f);
    return b;
}

static int is_accessory(const struct libusb_device_descriptor *d)
{
    return d->idVendor == 0x18d1 && d->idProduct >= 0x2d00 && d->idProduct <= 0x2d05;
}

static libusb_device_handle *find(int vid, int pid, int any, int accessory)
{
    libusb_device **list;
    ssize_t n = libusb_get_device_list(0, &list);
    libusb_device_handle *r = 0;
    for (ssize_t i = 0; i < n && !r; i++) {
        struct libusb_device_descriptor d;
        libusb_get_device_descriptor(list[i], &d);
        if (accessory ? is_accessory(&d) : (any ? d.bDeviceClass == 0 && d.idVendor != 0x1d6b : d.idVendor == vid && d.idProduct == pid))
            if (libusb_open(list[i], &r))
                r = 0;
    }
    libusb_free_device_list(list, 1);
    return r;
}

static int aoa_switch(libusb_device_handle *d)
{
    unsigned char v[2] = { 0 };
    int r = libusb_control_transfer(d, 0xc0, 51, 0, 0, v, 2, 1000);
    if (r != 2 || !(v[0] | v[1] << 8)) {
        printf("AOA: GET_PROTOCOL failed (%d): not an accessory-capable Android device\n", r);
        return -1;
    }
    printf("AOA: protocol %d\n", v[0] | v[1] << 8);
    static const char *const s[] = { "Android", "Android Auto", "Android Auto", "2.0.1", "https://example.invalid", "HU-AAAAAA001" };
    for (int i = 0; i < 6; i++)
        if (libusb_control_transfer(d, 0x40, 52, 0, (unsigned short)i, (unsigned char *)s[i], (unsigned short)(strlen(s[i]) + 1), 1000) < 0)
            return printf("AOA: SEND_STRING %d failed\n", i), -1;
    if (libusb_control_transfer(d, 0x40, 53, 0, 0, 0, 0, 1000) < 0)
        return printf("AOA: START failed\n"), -1;
    printf("AOA: started, waiting for the accessory to enumerate\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: aa_host VID:PID|any CERT.pem KEY.pem [seconds] [video.h264]\n");
        return 2;
    }
    int vid = 0, pid = 0, any = !strcmp(argv[1], "any"), secs = argc > 4 ? atoi(argv[4]) : 30;
    if (!any)
        sscanf(argv[1], "%x:%x", &vid, &pid);
    libusb_init(0);
    h = find(0, 0, 0, 1);
    if (!h) {
        libusb_device_handle *d = find(vid, pid, any, 0);
        if (!d) { printf("no phone found\n"); return 1; }
        if (aoa_switch(d)) return 1;
        libusb_close(d);
        for (int i = 0; i < 50 && !h; i++)
            usleep(100000), h = find(0, 0, 0, 1);
        if (!h) { printf("accessory did not appear\n"); return 1; }
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    if (libusb_claim_interface(h, 0)) { printf("claim interface 0 failed\n"); return 1; }
    struct libusb_config_descriptor *cfg;
    libusb_get_active_config_descriptor(libusb_get_device(h), &cfg);
    const struct libusb_interface_descriptor *id = &cfg->interface[0].altsetting[0];
    for (int i = 0; i < id->bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor *e = &id->endpoint[i];
        if ((e->bmAttributes & 3) == 2)
            *((e->bEndpointAddress & 0x80) ? &ep_in : &ep_out) = e->bEndpointAddress;
    }
    printf("accessory: bulk IN %02x OUT %02x\n", ep_in, ep_out);
    if (argc > 5)
        vf = fopen(argv[5], "wb");

    struct aa_platform p = { send_cb, video_cb, 0, log_cb, now_us, 0, 0, 0, 0, 160 };
    p.cert = slurp(argv[2], &p.cert_len);
    p.key = slurp(argv[3], &p.key_len);
    if (aa_start(&p)) return 1;
    unsigned long long end = now_us() + (unsigned long long)secs * 1000000;
    enum aa_state last = AA_IDLE;
    static const char *const names[] = { "idle", "version", "tls", "authed", "discovery", "running", "closed", "error" };
    unsigned char buf[16384];
    while (now_us() < end) {
        int got = 0;
        int r = libusb_bulk_transfer(h, ep_in, buf, sizeof buf, &got, 50);
        if (got > 0)
            aa_rx(buf, got);
        else if (r && r != LIBUSB_ERROR_TIMEOUT) {
            printf("bulk in: %s\n", libusb_error_name(r));
            break;
        }
        const struct aa_status *s = aa_status();
        if (s->state != last) {
            printf("state: %s (version %u.%u, channels open %u)\n", names[s->state], s->version_major, s->version_minor, s->channels_open);
            last = s->state;
            if (s->state == AA_ERROR || s->state == AA_CLOSED)
                break;
        }
    }
    const struct aa_status *s = aa_status();
    printf("done: state %s, rx frames %u msgs %u, tx frames %u, video frames %u (%u bytes), audio frames %u, unknown %u, errors %u\n",
           names[s->state], s->rx_frames, s->rx_msgs, s->tx_frames, s->video_frames, s->video_bytes, s->audio_frames, s->unknown, s->errors);
    if (vf)
        fclose(vf);
    return s->state == AA_RUNNING ? 0 : 1;
}

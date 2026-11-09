/* FatFs disk I/O glue (fatfs/diskio.h) onto sd.c. One drive (0), the SD card; sectors are absolute LBAs. */
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "sd.h"

static DSTATUS st = STA_NOINIT;

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : st; }

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv)
        return STA_NOINIT;
    if (st & STA_NOINIT)
        st = sd_init() ? STA_NOINIT : 0;
    return st;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (st & STA_NOINIT))
        return RES_NOTRDY;
    return sd_read(sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv || (st & STA_NOINIT))
        return RES_NOTRDY;
    return sd_write(sector, buff, count) ? RES_ERROR : RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)buff;
    if (pdrv || (st & STA_NOINIT))
        return RES_NOTRDY;
    return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;   /* writes are complete when sd_write returns */
}

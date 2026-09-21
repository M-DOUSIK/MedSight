/* sd_diskio.c - FatFs disk I/O glue onto the STM32 HAL SD driver.
 *
 * ── The per-sector UART tracing is now off by default ─────────
 *
 * This file printed FOUR lines per sector operation ("reading sector N",
 * "HAL_SD_ReadBlocks returned R", "waiting for transfer state...", "done"),
 * on the logger task's hot path. At 115200 baud with a blocking, polled UART
 * that is on the order of 13 ms of pure debug overhead per sector - which is
 * to say the debug output cost several times more than the SD transaction it
 * was describing, on every single log line, dispense record and gallery save.
 *
 * The tracing itself is genuinely valuable: it is what made the
 * VddIO5 power-domain bug and the early card
 * problems diagnosable. So it is guarded, not deleted - set
 * MEDSIGHT_DEBUG_DISKIO to 1 and it is all back, unchanged.
 *
 * Note the polling loops below already have iteration-count timeouts, per
 * this project's rule that a hardware polling loop must
 * never be able to spin forever. They were re-checked and left as
 * they are; what changed is that a timeout now reports itself even with
 * tracing off, because that is a genuine fault and not a trace line.
 */

#include "sd_diskio.h"
#include "ms_osal.h"   /* Osal_delay_ms() on the re-mount path */

/* SD handler declared in main.c or sd_logger.c */
#include <stdio.h>

/* Set to 1 to restore the per-sector read/write tracing. Default 0 - see the
 * file comment above for what it costs. */
#ifndef MEDSIGHT_DEBUG_DISKIO
#define MEDSIGHT_DEBUG_DISKIO   0
#endif

#if MEDSIGHT_DEBUG_DISKIO
#define DISKIO_TRACE(...)   printf(__VA_ARGS__)
#else
#define DISKIO_TRACE(...)   do { } while (0)
#endif

extern SD_HandleTypeDef hsd2;
static volatile DSTATUS Stat = STA_NOINIT;

DSTATUS disk_initialize(BYTE pdrv) {
    DISKIO_TRACE("disk_initialize: starting HAL_SD_Init...\r\n");
    if (pdrv != 0) {
        printf("disk_initialize: wrong pdrv %d\r\n", pdrv);
        return STA_NOINIT;
    }

    /* ── Re-initialising a live handle never works ─────────────
     *
     * Found on hardware: pull the card, push it back in, and every retry
     * printed "HAL_SD_Init failed (1)" forever. The card was fine; the
     * handle was the problem.
     *
     * HAL_SD_Init only performs the card power-up and identification
     * sequence from a handle in HAL_SD_STATE_RESET. After a successful boot
     * mount the handle is in HAL_SD_STATE_READY, so a retry skipped
     * MspInit, left the SDMMC powered in its old state, and tried to talk to
     * a card that had just been through a power cycle of its own and was
     * back in idle waiting for CMD0. It answered nothing, every time.
     *
     * HAL_SD_DeInit() puts the peripheral back in RESET (power off, state
     * cleared), so the Init below runs the full sequence exactly as it does
     * at boot. This is also why a removal is survivable at all: on the first
     * ever call the handle IS in RESET and this branch does nothing. */
    if (hsd2.State != HAL_SD_STATE_RESET) {
        DISKIO_TRACE("disk_initialize: handle not RESET, de-initialising first\r\n");
        (void)HAL_SD_DeInit(&hsd2);
        /* Let the card's own power-on reset settle before addressing it.
         * The SD spec allows 1 ms; 10 ms is free here (this runs on a mount
         * attempt, not a hot path) and covers slow cards.
         *
         * osal_delay_ms(), not HAL_Delay(): this runs on the logger task and
         * yields rather than busy-waiting, per the earlier rule. It is
         * only ever reached after a successful earlier init, so the kernel
         * is always running by the time it executes. */
        osal_delay_ms(10u);
    }

    HAL_StatusTypeDef status = HAL_SD_Init(&hsd2);
    DISKIO_TRACE("disk_initialize: HAL_SD_Init returned %d\r\n", status);

    if (status == HAL_OK) {
        Stat &= ~STA_NOINIT;
        return 0;
    }
    /* Not traced away: a failed card init is a real fault the operator needs
     * to see, and it happens once, not once per sector. */
    printf("disk_initialize: HAL_SD_Init failed (%d)\r\n", status);
    Stat |= STA_NOINIT;
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return Stat;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    DISKIO_TRACE("disk_read: reading sector %lu, count %u\r\n", (unsigned long)sector, count);
    if (pdrv != 0 || (Stat & STA_NOINIT)) return RES_NOTRDY;

    HAL_StatusTypeDef status = HAL_SD_ReadBlocks(&hsd2, buff, sector, count, 1000);
    DISKIO_TRACE("disk_read: HAL_SD_ReadBlocks returned %d\r\n", status);

    if (status == HAL_OK) {
        DISKIO_TRACE("disk_read: waiting for transfer state...\r\n");
        uint32_t timeout = 1000000;
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {
            timeout--;
            if (timeout == 0) {
                /* Always reported: a stuck card is a fault, not a trace. */
                printf("disk_read: timeout waiting for transfer state!\r\n");
                Stat |= STA_NOINIT;   /* force a re-init on the next attempt */
                return RES_ERROR;
            }
        }
        DISKIO_TRACE("disk_read: done\r\n");
        return RES_OK;
    }
    /* Card removed mid-session shows up here. sd_logger.c turns the resulting
     * FR_DISK_ERR into an unmount + retry rather than a permanent failure. */
    Stat |= STA_NOINIT;
    return RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    DISKIO_TRACE("disk_write: writing sector %lu, count %u\r\n", (unsigned long)sector, count);
    if (pdrv != 0 || (Stat & STA_NOINIT)) return RES_NOTRDY;

    HAL_StatusTypeDef status = HAL_SD_WriteBlocks(&hsd2, (uint8_t*)buff, sector, count, 1000);
    DISKIO_TRACE("disk_write: HAL_SD_WriteBlocks returned %d\r\n", status);

    if (status == HAL_OK) {
        DISKIO_TRACE("disk_write: waiting for transfer state...\r\n");
        uint32_t timeout = 1000000;
        while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {
            timeout--;
            if (timeout == 0) {
                printf("disk_write: timeout waiting for transfer state!\r\n");
                Stat |= STA_NOINIT;
                return RES_ERROR;
            }
        }
        DISKIO_TRACE("disk_write: done\r\n");
        return RES_OK;
    }
    Stat |= STA_NOINIT;
    return RES_ERROR;
}

/* Wait, with a bounded spin, for the card to finish any in-progress
 * programming and return to the transfer state. Same loop the read and write
 * paths above already use after each operation. */
static DRESULT wait_card_ready(void)
{
    uint32_t timeout = 1000000;
    while (HAL_SD_GetCardState(&hsd2) != HAL_SD_CARD_TRANSFER) {
        if (--timeout == 0u) {
            printf("disk_ioctl: timeout waiting for transfer state!\r\n");
            Stat |= STA_NOINIT;
            return RES_ERROR;
        }
    }
    return RES_OK;
}

/*
 * A fix: this function had returned RES_NOTRDY for EVERY command
 * for a long time, and nothing noticed.
 *
 * The guard was `if (HAL_SD_GetState(&hsd2) != HAL_SD_STATE_TRANSFER) return
 * RES_NOTRDY;`. HAL_SD_GetState() returns the HAL *driver* handle's State
 * field - not the card's status - and a grep of every assignment to that
 * field in stm32n6xx_hal_sd.c shows the driver only ever writes RESET, READY,
 * BUSY and PROGRAMMING to it. **HAL_SD_STATE_TRANSFER is never assigned at
 * all**, so the condition was unconditionally true. The intended check was
 * against HAL_SD_GetCardState()/HAL_SD_CARD_TRANSFER, which is what the read
 * and write paths above correctly use.
 *
 * Why it stayed hidden: FatFs calls disk_ioctl(CTRL_SYNC) from sync_fs(), at
 * the very END of f_close(), *after* the file data and directory entry have
 * already been written - and turns a non-RES_OK result into FR_DISK_ERR. So
 * every close of every file has been reporting a disk error while the data it
 * wrote was perfectly fine. Earlier builds' sd_logger.c discarded f_close()'s
 * return value entirely, so the error went straight to the floor; that is also
 * why patients.dat has always persisted correctly despite this.
 *
 * Checking those return values surfaced it - and
 * then made it worse, because sd_logger.c's new hot-plug handling classifies
 * FR_DISK_ERR as "the media went away" and unmounted a perfectly healthy card
 * on the first log line. That regression is this bug, not the hot-plug logic.
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    DRESULT res = RES_ERROR;
    HAL_SD_CardInfoTypeDef CardInfo;

    if (pdrv != 0) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            /* Flush: let the card finish programming before we tell FatFs the
             * sync completed. Nothing else is needed - this driver does no
             * write-back caching of its own. */
            res = wait_card_ready();
            break;
        case GET_SECTOR_COUNT:
            HAL_SD_GetCardInfo(&hsd2, &CardInfo);
            *(DWORD*)buff = CardInfo.LogBlockNbr;
            res = RES_OK;
            break;
        case GET_SECTOR_SIZE:
            HAL_SD_GetCardInfo(&hsd2, &CardInfo);
            *(WORD*)buff = CardInfo.LogBlockSize;
            res = RES_OK;
            break;
        case GET_BLOCK_SIZE:
            HAL_SD_GetCardInfo(&hsd2, &CardInfo);
            *(DWORD*)buff = CardInfo.LogBlockSize / 512;
            res = RES_OK;
            break;
        default:
            res = RES_PARERR;
    }
    return res;
}

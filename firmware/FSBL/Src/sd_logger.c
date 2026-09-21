/* sd_logger.c - MedSight SD card logging (OSAL async queue)
 *
 * Two-layer design:
 *   Layer 1 (synchronous): SD_Log_Event(), SD_Log_Binary() - direct FATFS
 *     writes. Called only from main() before scheduler, or from task_logger.
 *   Layer 2 (async queue): SD_Log_Event_Async() -> posts to ms_log_queue ->
 *     task_logger_fn() drains and calls Layer 1.
 *
 * No FreeRTOS API is called here. All RTOS operations go through ms_osal.h.
 *
 * ── Mounting became lazy, and media loss became survivable ────
 *
 * The card used to be mounted exactly once, at the top of
 * task_logger_fn(), and `is_mounted` never went back to false. Two real
 * consequences:
 *
 *   1. Pull the card out mid-session and every subsequent operation failed
 *      silently forever - f_open() returned FR_DISK_ERR, the caller got
 *      `false`, and nothing ever tried to mount again even after the card was
 *      put back. The dispense/registration flows kept running and kept
 *      believing they had logged.
 *   2. A start-up ordering hazard. gallery_init() reads patients.dat during
 *      AI bring-up, and there was nothing making that happen after the logger
 *      task's one-shot mount - the AI task runs at a higher priority than the
 *      logger, so on a cold boot the enrolled patient gallery could be read
 *      before the filesystem existed and come back empty.
 *
 * Both are fixed by the same change: every entry point calls
 * sd_ensure_mounted() first, which mounts on demand and re-mounts after a
 * failure (rate-limited so a genuinely absent card costs one f_mount attempt
 * per second, not one per log line). Errors that mean "the media went away"
 * drop the mount so the next call retries. Nothing here ever calls
 * Error_Handler() - an SD fault degrades the device, it does not stop it.
 */

#include "sd_logger.h"
#include "schedule_time_source.h"   /* Log timestamps */
#include "ms_osal.h"
#include "ff.h"
#include "stm32n6xx_hal.h"   /* HAL_GetTick, for the remount back-off */
#include <string.h>
#include <stdio.h>

/* ── FATFS state ─────────────────────────────────────────────────────────── */
static FATFS  SDFatFs;
static bool   is_mounted = false;

/* How long to wait before retrying a failed mount. One second is
 * short enough that re-inserting the card recovers within one user action and
 * long enough that a permanently empty slot does not turn every log line into
 * an SDMMC transaction. */
#define SD_REMOUNT_RETRY_MS   1000u
static uint32_t s_last_mount_attempt = 0u;
static bool     s_mount_attempted    = false;
static uint32_t s_fault_count        = 0u;

/* ── Async queue handle ──────────────────────────────────────────────────── */
/* Created by SD_Logger_Queue_Init() before the scheduler starts.            */
#define LOG_QUEUE_DEPTH  16   /* 16 slots × 128 bytes = 2 KB               */
static osal_queue_handle_t s_log_queue = NULL;

/* ════════════════════════════════════════════════════════════════════════════
 * Mount management
 * ════════════════════════════════════════════════════════════════════════════ */

/* FRESULT values that mean "the media is gone or unusable", as opposed to
 * "that particular file was not there". Only these drop the mount. */
static bool fres_is_media_fault(FRESULT res)
{
    switch (res) {
        case FR_DISK_ERR:
        case FR_NOT_READY:
        case FR_NO_FILESYSTEM:
        case FR_INVALID_DRIVE:
        case FR_TIMEOUT:
            return true;
        default:
            return false;
    }
}

/* Drop the mount so the next operation re-mounts. Called on a media fault. */
static void sd_note_media_fault(const char *where, FRESULT res)
{
    s_fault_count++;
    if (is_mounted) {
        is_mounted = false;
        (void)f_mount(NULL, "", 0);   /* release FatFs' own state */
        printf("SD: media fault in %s (FRESULT %d) - card unmounted, "
               "will retry.\r\n", where, (int)res);
    }
}

/* Mount if not mounted. Returns true when the filesystem is usable.
 * Rate-limited: at most one f_mount attempt per SD_REMOUNT_RETRY_MS. */
static bool sd_ensure_mounted(void)
{
    if (is_mounted) {
        return true;
    }

    uint32_t now = HAL_GetTick();
    if (s_mount_attempted && ((now - s_last_mount_attempt) < SD_REMOUNT_RETRY_MS)) {
        return false;
    }
    s_last_mount_attempt = now;

    FRESULT res = f_mount(&SDFatFs, "", 1);
    if (res == FR_OK) {
        is_mounted = true;
        if (s_mount_attempted) {
            printf("SD: card remounted OK.\r\n");
        } else {
            printf("SD_Logger_Init: SD card mounted OK.\r\n");
        }
        s_mount_attempted = true;
        return true;
    }

    if (!s_mount_attempted) {
        printf("SD_Logger_Init: f_mount failed, error %d\r\n", (int)res);
    }
    s_mount_attempted = true;
    return false;
}

bool SD_Logger_Is_Available(void)
{
    return is_mounted;
}

uint32_t SD_Logger_Get_Fault_Count(void)
{
    return s_fault_count;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Synchronous API (Layer 1)
 * ════════════════════════════════════════════════════════════════════════════ */

bool SD_Logger_Init(void)
{
    printf("SD_Logger_Init: Mounting SD card...\r\n");
    return sd_ensure_mounted();
}

/* ── Timestamps ───────────────────────────────────────────────
 *
 * An adherence audit trail whose lines have no time on them answers "did a
 * dose happen" and not "when". The second question is the one a carer is
 * actually asking, and until recently the device had no clock to answer
 * it with.
 *
 * The rule is deliberately conditional: a line is stamped IF AND ONLY IF a
 * carer has set the clock. Before that the RTC is counting from its power-on
 * default, and a line stamped "2000-01-01 00:04" is worse than an unstamped
 * one because it looks like real data. time_source_format_stamp() returns an
 * empty string in that case, so this concatenation costs nothing and the two
 * eras are distinguishable at a glance in the file.
 *
 * This is a module-boundary crossing worth naming: sd_logger.c now depends on
 * schedule_time_source.h. It goes through that module's public API and never
 * touches the RTC directly, which is exactly what
 * the technical reference's rule requires - the logger is simply the
 * second consumer rather than the first. */
static void log_stamp(char *out, size_t n, const char *event_string)
{
    char stamp[32];
    time_source_format_stamp(stamp, sizeof(stamp));
    snprintf(out, n, "%s%s", stamp, event_string);
}

bool SD_Log_Event(const char *event_string)
{
    /* Stamped once, up front, so the line that reaches the card and the line
     * that reaches the UART are byte-identical. Building it twice would let
     * them disagree across a minute boundary, and a log you cannot reconcile
     * with the console is worse than either alone.
     *
     * Sized for SD_LOG_MSG_MAX_LEN plus the stamp; snprintf truncates rather
     * than overflowing if a caller ever passes something longer. */
    char line[SD_LOG_MSG_MAX_LEN + 32];
    log_stamp(line, sizeof(line), event_string);

    if (!sd_ensure_mounted()) {
        return false;
    }

    FIL file;
    FRESULT res = f_open(&file, "events.log", FA_OPEN_APPEND | FA_WRITE);
    if (res != FR_OK) {
        printf("SD_Log_Event: f_open failed %d\r\n", res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Log_Event", res);
        return false;
    }

    UINT bw = 0;
    res = f_write(&file, line, strlen(line), &bw);
    if (res == FR_OK) {
        res = f_write(&file, "\n", 1, &bw);
    }
    FRESULT cres = f_close(&file);
    if (res == FR_OK) res = cres;

    if (res != FR_OK) {
        printf("SD_Log_Event: write failed %d\r\n", res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Log_Event", res);
        return false;
    }

    printf("SD_Log_Event: logged: %s\r\n", line);
    return true;
}

bool SD_Log_Binary(const uint8_t *data, uint32_t length)
{
    if (!sd_ensure_mounted()) {
        return false;
    }

    FIL file;
    /* Self-test blob. Kept because it is the one write that
     * happens unconditionally at boot, which makes it a useful "is the card
     * actually writable" probe - see task_logger_fn(). */
    FRESULT res = f_open(&file, "dummy_face.bin", FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        printf("SD_Log_Binary: f_open failed %d\r\n", res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Log_Binary", res);
        return false;
    }

    UINT bw = 0;
    res = f_write(&file, data, length, &bw);
    FRESULT cres = f_close(&file);
    if (res == FR_OK) res = cres;

    if (res != FR_OK) {
        printf("SD_Log_Binary: write failed %d\r\n", res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Log_Binary", res);
        return false;
    }

    printf("SD_Log_Binary: %lu bytes written.\r\n", (unsigned long)length);
    return (bw == length);
}

bool SD_Write_File(const char *filename, const uint8_t *data, uint32_t length)
{
    if (!sd_ensure_mounted()) {
        printf("SD_Write_File(%s): card unavailable.\r\n", filename);
        return false;
    }

    FIL file;
    FRESULT res = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        printf("SD_Write_File(%s): f_open failed %d\r\n", filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Write_File", res);
        return false;
    }

    UINT bw = 0;
    res = f_write(&file, data, length, &bw);
    FRESULT cres = f_close(&file);
    if (res == FR_OK) res = cres;

    if (res != FR_OK) {
        printf("SD_Write_File(%s): write failed %d\r\n", filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Write_File", res);
        return false;
    }

    /* Report bw, not length. This used to print the REQUESTED size and then
     * return (bw == length), so a short write logged a cheerful "1632 bytes
     * written" and failed silently - the one case where this line matters
     * was the one case it lied about. FatFs returns FR_OK with bw < length
     * when the card is full. */
    if (bw != length) {
        printf("SD_Write_File(%s): SHORT WRITE - %lu of %lu bytes (card full?)\r\n",
               filename, (unsigned long)bw, (unsigned long)length);
        return false;
    }
    printf("SD_Write_File(%s): %lu bytes written.\r\n", filename,
           (unsigned long)bw);
    return true;
}

bool SD_Read_File(const char *filename, uint8_t *out_data, uint32_t max_length,
                   uint32_t *out_bytes_read)
{
    if (out_bytes_read) *out_bytes_read = 0;
    if (!sd_ensure_mounted()) {
        printf("SD_Read_File(%s): card unavailable.\r\n", filename);
        return false;
    }

    FIL file;
    FRESULT res = f_open(&file, filename, FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK) {
        /* Not found is expected on first boot (no gallery saved yet) - not an error. */
        printf("SD_Read_File(%s): f_open failed %d (not found?)\r\n", filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Read_File", res);
        return false;
    }

    UINT br = 0;
    res = f_read(&file, out_data, max_length, &br);
    FRESULT cres = f_close(&file);
    if (res == FR_OK) res = cres;

    if (res != FR_OK) {
        printf("SD_Read_File(%s): f_read failed %d\r\n", filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Read_File", res);
        return false;
    }

    if (out_bytes_read) *out_bytes_read = br;
    printf("SD_Read_File(%s): %u bytes read.\r\n", filename, br);
    return true;
}

/* ── Reading the END of the log, not the beginning ────────────
 *
 * SD_Read_File() above reads a whole file into a caller-sized buffer and
 * fails if it does not fit. That is right for patients.dat, which has a
 * fixed size. It is wrong for events.log, which is append-only and grows
 * without limit - after a few weeks of use, "read the whole log" is neither
 * possible in the RAM this device has nor what anyone wants: a carer
 * reviewing adherence wants the most RECENT entries.
 *
 * So this seeks to (size - max_length) and reads forward. The first line in
 * the buffer will usually be a partial line; the caller is expected to
 * discard everything up to the first '
', which carer_ui.c does.
 */
bool SD_Read_File_Tail(const char *filename, uint8_t *out_data,
                        uint32_t max_length, uint32_t *out_bytes_read)
{
    if (out_bytes_read) *out_bytes_read = 0;
    if (!sd_ensure_mounted()) {
        printf("SD_Read_File_Tail(%s): card unavailable.\r\n", filename);
        return false;
    }

    FIL file;
    FRESULT res = f_open(&file, filename, FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK) {
        printf("SD_Read_File_Tail(%s): f_open failed %d (not found?)\r\n",
               filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Read_File_Tail", res);
        return false;
    }

    FSIZE_t size = f_size(&file);
    FSIZE_t from = (size > (FSIZE_t)max_length) ? (size - (FSIZE_t)max_length) : 0u;
    res = f_lseek(&file, from);

    UINT br = 0;
    if (res == FR_OK) {
        res = f_read(&file, out_data, max_length, &br);
    }
    FRESULT cres = f_close(&file);
    if (res == FR_OK) res = cres;

    if (res != FR_OK) {
        printf("SD_Read_File_Tail(%s): read failed %d\r\n", filename, res);
        if (fres_is_media_fault(res)) sd_note_media_fault("SD_Read_File_Tail", res);
        return false;
    }

    if (out_bytes_read) *out_bytes_read = br;
    printf("SD_Read_File_Tail(%s): %u bytes read from offset %lu of %lu.\r\n",
           filename, br, (unsigned long)from, (unsigned long)size);
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Async API (Layer 2) - backed by OSAL queue
 * ════════════════════════════════════════════════════════════════════════════ */

void SD_Logger_Queue_Init(void)
{
    s_log_queue = osal_queue_create(LOG_QUEUE_DEPTH, sizeof(ms_log_msg_t));
    /* osal_queue_create() calls Error_Handler() itself if the pool is
     * exhausted - no need to re-check for NULL here. */
}

void SD_Log_Event_Async(const char *event_string)
{
    if (s_log_queue == NULL) {
        /* Queue not initialised - scheduler not started yet.
         * Fall back to synchronous write. */
        SD_Log_Event(event_string);
        return;
    }

    ms_log_msg_t msg;
    /* Safe truncating copy - always NUL-terminates */
    size_t len = strlen(event_string);
    if (len >= SD_LOG_MSG_MAX_LEN) {
        len = SD_LOG_MSG_MAX_LEN - 1u;
    }
    memcpy(msg.text, event_string, len);
    msg.text[len] = '\0';

    /* Non-blocking send - drop silently if queue is full.
     * Log-queue overflow means the logger task is starved, which is a
     * priority-tuning issue, not a fatal error. */
    (void)osal_queue_send(s_log_queue, &msg, OSAL_NO_WAIT);
}

/**
 * @brief  Logger task body - blocks on the queue and writes to SD.
 *
 * Priority: 2 (below UI/camera/AI, above the heartbeat).
 * Stack:    1024 words - FATFS local buffers are significant.
 */
void task_logger_fn(void *arg)
{
    (void)arg;
    ms_log_msg_t msg;

    printf("task_logger: started.\r\n");

    /* Mount here rather than in main() because HAL_SD/FATFS need the kernel
     * tick running for their timeouts. This is no longer the
     * ONLY place a mount can happen - sd_ensure_mounted() does it on demand
     * from any entry point - but doing it eagerly here still gets the card up
     * before the first log line and reports the result on the console. */
    if (SD_Logger_Init()) {
        /* Distinct from task_ui_fn's queued boot line briefly gave
         * both the same text, which made one boot look like two in the log. */
        SD_Log_Event("System Boot - SD logger ready");
        /* Write-path probe: proves the card is not just mounted but writable
         * (a write-protected or worn card mounts fine and then fails). */
        uint8_t dummy_face[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
        SD_Log_Binary(dummy_face, sizeof(dummy_face));
    } else {
        printf("task_logger: SD mount failed - continuing without logging, "
               "will retry on next write.\r\n");
    }

    for (;;) {
        /* Block until a log message arrives (or wake every 5 s as a watchdog).
         * The watchdog wake-up is what gives a re-inserted card a chance to
         * come back even on an otherwise idle device. */
        if (osal_queue_receive(s_log_queue, &msg, 5000u)) {
            SD_Log_Event(msg.text);
        } else if (!is_mounted) {
            (void)sd_ensure_mounted();
        }
    }
}

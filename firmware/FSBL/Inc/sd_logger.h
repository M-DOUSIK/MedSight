#ifndef __SD_LOGGER_H
#define __SD_LOGGER_H

#include <stdint.h>
#include <stdbool.h>

/* ── Log message structure used by the async queue ──────────────────────── */
/* Keep this compact: the queue stores copies of this struct.               */
#define SD_LOG_MSG_MAX_LEN  128

typedef struct {
    char text[SD_LOG_MSG_MAX_LEN];
} ms_log_msg_t;

/* ════════════════════════════════════════════════════════════════════════════
 * Synchronous API
 * Caller blocks until the SD operation completes.
 * Use ONLY from main() (before scheduler starts) or from task_logger itself.
 * ════════════════════════════════════════════════════════════════════════════ */

/** @brief Mount FATFS and open the log infrastructure.
 *  Mounting is now also lazy - every read/write entry point below
 *  mounts on demand and re-mounts (rate-limited) after a media fault - so
 *  this is an eager convenience, not a precondition. Callers no longer have
 *  to reason about whether the logger task has run yet. */
bool SD_Logger_Init(void);

/** @brief True if the filesystem is currently mounted and usable.
 *  Lets the UI state what is wrong (STATE_ALERT / the home
 *  screen's status line) instead of dispensing and registering as if
 *  everything were being recorded when nothing is. */
bool SD_Logger_Is_Available(void);

/** @brief Number of media faults seen since boot (card pulled, write error,
 *  filesystem gone). Non-zero means at least one event may not have been
 *  recorded - worth surfacing rather than hiding. */
uint32_t SD_Logger_Get_Fault_Count(void);

/** @brief Append a text event line to events.log. Blocking. */
bool SD_Log_Event(const char* event_string);

/** @brief Write a binary blob to dummy_face.bin. Blocking. */
bool SD_Log_Binary(const uint8_t* data, uint32_t length);

/** @brief Write a binary blob to an arbitrary named file (overwrite). Blocking.
 *  Used by ai_vision.c's gallery_save() for patients.dat - kept
 *  here (not in ai_vision.c) per the technical reference rule that only
 *  sd_logger.c touches FATFS/SDMMC directly. */
bool SD_Write_File(const char* filename, const uint8_t* data, uint32_t length);

/** @brief Read a named file fully into out_data (capacity max_length). Blocking.
 *  Returns false if the file does not exist, can't be opened, or is larger
 *  than max_length. On success, *out_bytes_read is the file size. */
bool SD_Read_File(const char* filename, uint8_t* out_data, uint32_t max_length,
                   uint32_t* out_bytes_read);

/** @brief Read the LAST max_length bytes of a named file. Blocking.
 *  Carer mode's log review reads events.log, which is
 *  append-only and unbounded, so reading it whole is neither possible nor
 *  useful - the recent end is the part a carer cares about. The first line
 *  in the returned buffer is usually truncated; the caller discards up to
 *  the first newline. */
bool SD_Read_File_Tail(const char* filename, uint8_t* out_data,
                        uint32_t max_length, uint32_t* out_bytes_read);

/* ════════════════════════════════════════════════════════════════════════════
 * Asynchronous API
 * Non-blocking: copies the message into the log queue and returns.
 * task_logger drains the queue and writes to SD in its own context.
 * Safe to call from any task or interrupt context that is at or below
 * configMAX_SYSCALL_INTERRUPT_PRIORITY.
 * ════════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Initialise the inter-task log queue.
 *         Call once from main() before creating tasks.
 */
void SD_Logger_Queue_Init(void);

/**
 * @brief  Post a log message to the async queue.
 *         Non-blocking (OSAL_NO_WAIT). If the queue is full the message is
 *         silently dropped - log queue overflow is a diagnostic symptom, not
 *         a fatal error (the device keeps running).
 * @param  event_string  Null-terminated string ≤ SD_LOG_MSG_MAX_LEN-1 chars.
 */
void SD_Log_Event_Async(const char* event_string);

/**
 * @brief  Task body for the logger task.
 *         Blocks on the queue and calls SD_Log_Event() for each message.
 *         Never returns - create as an RTOS task via osal_task_create().
 * @param  arg  Unused (NULL).
 */
void task_logger_fn(void *arg);

#endif /* __SD_LOGGER_H */

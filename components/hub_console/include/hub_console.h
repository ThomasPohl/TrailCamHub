#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Serial command console on the console port, the same port as the logs: the
 * console UART, or the USB-Serial-JTAG port on boards whose console is there
 * (the T-Dongles). Commands are line based (see command_parser.hpp); every
 * reply line starts with "TCH " so the web installer and scripts can pick it
 * out of the log stream:
 *
 *   TCH READY <state>         the console started
 *   TCH OK <message>          command accepted
 *   TCH ERR <message>         command rejected
 *   TCH CONFIG <key> <value>  one setting (secrets hidden)
 *   TCH STATE <state> ...     controller state
 *   TCH INFO <text>           a line of a status, listing or benchmark
 *   TCH FILE <size>, TCH DATA <offset> <base64>, TCH CRC <offset> <length>
 *   <crc32>                   a file's contents (cat)
 *   TCH DONE                  the command is complete
 *   TCH EVENT <name> ...      asynchronous progress (from the controller)
 *
 * The diagnostic commands (heap, ls, cat, rm, sdbench) are built with
 * CONFIG_HUB_CONSOLE_DIAGNOSTICS.
 */
esp_err_t hub_console_start(void);

/*
 * Reports the cycle's phase for the status command and for refusing to
 * delete files while a cycle uses the card: one of "idle", "searching",
 * "connecting", "connected", "retry_wait", "uploading", "finished"; `ssid` may
 * be empty. Safe to call from any task.
 */
void hub_console_set_status(const char *phase, const char *ssid,
                            uint32_t retry_delay_ms);

/* Reports the media download progress for the status command. */
void hub_console_set_media_progress(uint32_t done, uint32_t stored,
                                    uint32_t total);

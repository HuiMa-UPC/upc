#ifndef UPC_MESSAGE_PATHS_HPP
#define UPC_MESSAGE_PATHS_HPP

/**
 * 消息中心（报警历史）落盘目录。
 *
 * 刻意不放在 UPC_MEDIA_ROOT 下：报警历史不是媒体。
 *
 *   /data/messages/
 *     alarm_history.jsonl
 */

#ifndef UPC_MESSAGE_DIR
#define UPC_MESSAGE_DIR  "/data/messages"
#endif

#ifndef UPC_ALARM_HISTORY_PATH
#define UPC_ALARM_HISTORY_PATH  UPC_MESSAGE_DIR "/alarm_history.jsonl"
#endif

#ifndef UPC_ALARM_HISTORY_PATH_TMP
#define UPC_ALARM_HISTORY_PATH_TMP  UPC_ALARM_HISTORY_PATH ".tmp"
#endif

#endif /* UPC_MESSAGE_PATHS_HPP */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Comandi */
#define C2_TAG_EXEC          0x01
#define C2_TAG_STDOUT        0x02
#define C2_TAG_STDERR        0x03
#define C2_TAG_RC            0x04
#define C2_TAG_PING          0x05
#define C2_TAG_PONG          0x06
#define C2_TAG_EXIT          0x07

/* Upload PC1 -> PC2 */
#define C2_TAG_UPLOAD_START  0x08
#define C2_TAG_UPLOAD_CHUNK  0x09
#define C2_TAG_UPLOAD_END    0x0A
#define C2_TAG_UPLOAD_RC     0x0B

/* Download PC2 -> PC1 */
#define C2_TAG_DOWNLOAD_START 0x0C
#define C2_TAG_DOWNLOAD_CHUNK 0x0D
#define C2_TAG_DOWNLOAD_END   0x0E
#define C2_TAG_DOWNLOAD_RC    0x0F

/* Controllo download (nuovo) */
#define C2_TAG_ABORT_DOWNLOAD 0x10   /* PC1 -> PC2: interrompi il download in corso */

#define C2_MSG_MAX 1024

typedef struct {
    uint8_t tag;
    size_t  len;
    uint8_t data[C2_MSG_MAX];
} c2_msg_t;
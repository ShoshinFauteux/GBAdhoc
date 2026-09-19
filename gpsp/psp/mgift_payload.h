#ifndef MGIFT_PAYLOAD_H
#define MGIFT_PAYLOAD_H
#include <stdint.h>
#define MGIFT_CARD_BYTES 332u
#define MGIFT_SCRIPT_MAX 512u
#define MGIFT_HEADER_BYTES 16u
#define MGIFT_PARCEL_MAX (MGIFT_HEADER_BYTES + MGIFT_CARD_BYTES + 2 * MGIFT_SCRIPT_MAX)
typedef struct {
   uint32_t id;
   uint16_t frlg_size, emerald_size;
   uint8_t card[MGIFT_CARD_BYTES];
   uint8_t frlg[MGIFT_SCRIPT_MAX], emerald[MGIFT_SCRIPT_MAX];
} mgift_parcel;
/* 0 = valid, -1 = invalid; output is untouched on failure. */
int mgift_parcel_decode(mgift_parcel *out, const uint8_t *data, unsigned size);
void mgift_parcel_ack(uint8_t out[12], uint32_t id, unsigned result);
#endif

#ifndef MICROVISOR_H
#define MICROVISOR_H
#include <stdint.h>
#include "mem_layout.h"

#define METADATA_OFFSET APP_META
#define PAGE_SIZE 256
#define HASH_MAP_SIZE 32
void load_image(uint8_t *page_buf, uint16_t offset);
uint8_t verify_activate_image();
void remote_attestation(uint8_t *mac);
int8_t parse_att_msg(const uint8_t *msg, uint8_t msg_length, uint8_t *result_msg, uint8_t mem_changed, uint8_t *metadata, uint8_t *prev_mem_state);
int8_t device_auth(uint8_t *MAC, uint8_t *update_req_msg, uint8_t *metadata, uint16_t *prover_id_map);
void map_init(uint16_t *map);

#endif

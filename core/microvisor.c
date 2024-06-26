#include "microvisor.h"
#include "virt_i.h"
#include <avr/boot.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <string.h>
#include "bootloader_progmem.h"
#include "mem_layout.h"
// #include "hmac-sha1.h"
#include "hmac-sha256.h"
#include "string_boot.h"

/* Sets some ELF metadata (not strictly required) */
#include <avr/signature.h>
#include <avr/fuse.h>
/* External crystal osc as clock, >8 MHz, minimum wake-up delay (258CK),
 * maximum additional reset delay (14CK+65ms). SPI and EESAVE enabled. Brownout
 * on 2.7V. FUSE_BOOTSZ is defined in mem_layout.h */
#define BOOTSIZE 4
//FUSES = {
 // .low = (FUSE_SUT1 & FUSE_CKSEL0),
  //.high = (FUSE_SPIEN & FUSE_EESAVE & FUSE_BOOTSZ),
  //.extended = (FUSE_BODLEVEL1),
//};

/* Store all word addresses of valid microvisor entrypoints, terminated with
 * 0x0000 */
BOOTLOADER_PROGMEM
const uint16_t uvisor_entrypoints[] = {
    (uint16_t) &safe_icall_ijmp,
    (uint16_t) &safe_ret,
    (uint16_t) &safe_reti,
    (uint16_t) &load_image,
    (uint16_t) &verify_activate_image,
    (uint16_t) &parse_att_msg,
    (uint16_t) &device_auth,
    (uint16_t) &map_init,
    0x0000
};

 BOOTLOADER_PROGMEM static const uint8_t key_hmac[] = {0x6e, 0x26, 0x88, 0x6e,
    0x4e, 0x07, 0x07, 0xe1, 0xb3, 0x0f, 0x24, 0x16, 0x0e, 0x99, 0xb9, 0x12,
    0xe4, 0x61, 0xc4, 0x24, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01};

// BOOTLOADER_PROGMEM uint8_t metadata[HASH_MAP_SIZE] = {0};
// BOOTLOADER_PROGMEM uint16_t prover_id_map[HASH_MAP_SIZE] = {0};
// BOOTLOADER_PROGMEM uint8_t prev_mem_state[32] = {0};
// BOOTLOADER_SECTION uint16_t self_id = 1000;

// BOOTLOADER_PROGMEM uint8_t mem_changed = 1;

/****************************************************************************/
/*                      MICROVISOR HELPER FUNCTIONS                         */
/****************************************************************************/

/* Verifies if _WORD_ address is safe to jump to for the app image to be
 * deployed */
BOOTLOADER_SECTION static uint8_t
verify_target_deploy(uint16_t target) {
  uint16_t address;
  uint16_t addr_ptr;
  uint8_t i;

  /* If everything is in upper progemem: 17th bit should be 1 for all these reads in case of more than 64kB of Flash
   * */
  //RAMPZ = 0x01;

  /* Get .data byte address (= end of app .text) and convert it to a word
   * address */
  address = pgm_read_word_near(SHADOW_META + 2);
  address >>= 1;
  if(target >= address) { /* Target is not inside app .text */
    /* Init pointer to entrypoint array in progmem */
    addr_ptr = (uint16_t) uvisor_entrypoints;
    do {
      address = pgm_read_word_near(addr_ptr);
      if(target == address)
        return 1; /* Success, target is entrypoint to uvisor */
      addr_ptr += 2;
    } while(address != 0x0000);

    /* Target outside of app .text + not an uvisor entrypoint */
    return 0;
  } else { /* Target is inside app .text */
    i = (uint8_t) pgm_read_word_near(SHADOW_META + 4);
    addr_ptr = (uint16_t) SHADOW_META + 6;
    while(i > 0) {
      address = pgm_read_word_near(addr_ptr);
      if(target == address)
        return 0; /* Fail, target is unsafe 2nd word */
      addr_ptr += 2;
      i--;
    }

    /* Target inside of app .text + not an unsafe 2nd word --> success */
    return 1;
  }
}

/* Writes to arbitrary page of progmem */
BOOTLOADER_SECTION static void
write_page(uint8_t *page_buf, uint32_t offset) {
  uint32_t pageptr;
  uint8_t i;

  /* Erase page */
  boot_page_erase(offset);
  boot_spm_busy_wait();

  /* Write a word (2 bytes) at a time */
  pageptr = offset;
  i = PAGE_SIZE/2;
  do {
	uint16_t w = *page_buf++;
	w |= (*page_buf++) << 8;
	boot_page_fill(pageptr, w);
    pageptr += 2;
  } while(i -= 1);
  boot_page_write(offset);     // Store buffer in flash page.
  boot_spm_busy_wait();        // Wait until the memory is written.

  /* Reenable RWW-section again. We need this if we want to jump back to the
   * application after bootloading. */
  boot_rww_enable();
}

/* Loads key into buff */

BOOTLOADER_SECTION static inline void
load_key(uint8_t *buff, uint8_t *secret) {
  uint8_t i;
  // Read page byte at a time
  for(i=0; i<32; i++) {
    buff[i] = pgm_read_byte_near(secret +i);
  }
}

/* Reads arbitrary page from progmem */

BOOTLOADER_SECTION static inline void
read_page(uint8_t *block_buf, uint32_t offset) {
  uint16_t i;
  /* Read page byte at a time at a time */
  for(i=0; i<PAGE_SIZE; i++) {
    block_buf[i] = pgm_read_byte_near(offset);
    offset += 1;
  }
}

/* Activates image by transfering image from deploy to running app space */
BOOTLOADER_SECTION static inline void
switch_image() {
  uint16_t pages;
  uint16_t i;
  uint8_t buf[PAGE_SIZE];

  /* Calculate amount of pages to copy */
  //RAMPZ = 0x01;
  pages = pgm_read_word_near(SHADOW_META);
  pages = pages/PAGE_SIZE + (pages%PAGE_SIZE > 0);

  for(i=0; i<pages; i++) {
    read_page(buf, ((uint32_t) SHADOW) + PAGE_SIZE*i);
    write_page(buf, PAGE_SIZE*i);
  }

  /* Copy metadata page */
  read_page(buf, SHADOW_META);
  write_page(buf, APP_META);
}

BOOTLOADER_SECTION static inline uint8_t
verify_shadow() {
  uint16_t current_word;
  uint16_t current_addr; //In WORDS, not bytes
  uint16_t pointer;
  //uint8_t pointer_rampz;
  uint16_t text_size; //In WORDS, not bytes
  uint8_t prev_op_long;

  /* Init text_size variable */
  //RAMPZ = 0x01;
  text_size = pgm_read_word_near(SHADOW_META + 2);
  text_size >>= 1;

  /* Loop over instructions word by word */
  //pointer_rampz = 0x00;
  pointer = SHADOW;
  current_addr = 0x0000;
  prev_op_long = 0;
  while(current_addr < text_size) {
    /* Fetch next word */
   // RAMPZ = pointer_rampz;
    current_word = pgm_read_word_near(pointer);

    /* Check target address of previous long op is correct */
    if(prev_op_long && !verify_target_deploy(current_word))
      return 0;

    /* Parse word as instruction and check if it is allowed. If this word is a
     * long call address (prev_op_long == 1), check if absent from list (i.e.
     * verify_target says we CAN jump to it) before rejecting */
    if((current_word == 0x940C) || (current_word == 0x940E)) {
      /* ---LONG INSTRUCTIONS WITH TARGET AS 2ND WORD--- */
      /* If target addr of long call gets decoded as long call and is not on
       * list, reject */
      if(prev_op_long && verify_target_deploy(current_addr))
        return 0;
      /* Set prev_op_long flag correctly: 1 in the normal case, and 0 if this
       * is the address of a long call. */
      prev_op_long = !prev_op_long;
    } else {
      /* ---NORMAL INSTRUCTIONS--- */

      /* Ops which need a relative address calculated set this flag. Signed
       * offset is stored in current_word */
      uint8_t calc_rel = 0;
      if((current_word == 0x9508)
          || (current_word == 0x9518)
          || (current_word == 0x9409)
          || (current_word == 0x9509)
          || (current_word == 0x95C8)
          || (current_word & 0xFE0F) == 0x9004
          || (current_word & 0xFE0F) == 0x9005) {
        /* PLAIN UNSAFE OPS: RET, RETI, IJMP, ICALL, LPM, LPM RD,Z(+) */
        /* In normal situation, reject. As target address of long call, reject
         * if not on list. */
        if( !prev_op_long || (prev_op_long && verify_target_deploy(current_addr)) )
          return 0;
      } else if((current_word & 0xFC00) == 0xF000
          || (current_word & 0xFC00) == 0xF400) {
        /* BRANCH OPS: extract offset and make signed 16 bit int */
        current_word &= 0x03F8;
        current_word >>= 3;
        if(current_word > 0x3F)
          current_word |= 0xFF80;
        calc_rel = 1;
      } else if((current_word & 0xF000) == 0xC000
          || (current_word & 0xF000) == 0xD000) {
        /* RJMP OR RCALL: extract offset and make signed 16 bit int */
        current_word &= 0x0FFF;
        if(current_word > 0x07FF)
          current_word |= 0xF000;
        calc_rel = 1;
      }

      /* Calculate target and check it */
      if(calc_rel) {
        current_word = current_addr + ((int16_t) current_word) + 1;
        if(!verify_target_deploy(current_word)) {
          /* In normal situation, reject. As target address of long call,
           * reject if not on list. */
          if( !prev_op_long || (prev_op_long && verify_target_deploy(current_addr)) )
            return 0;
        }
      }

      /* Set prev_op_long flag accordingly */
      prev_op_long = 0;
    }

    /* Increment loop variables */
    current_addr++;
    //if((pointer += 2) == 0x0000)
      //pointer_rampz = 0x01;
  }

  return 1;
}

BOOTLOADER_SECTION static inline uint8_t
verify_hmac() {
  uint16_t image_size; //in BYTES
  uint8_t meta_size = 3; //in BYTES, without digest
  uint8_t digest[32]; //HMAC_SHA1_BYTES

  uint32_t offset = SHADOW;
  hmac_sha256_ctx_t ctx;
  uint8_t buff[PAGE_SIZE*2]; //Twice pagesize to simplify hashing

  /* Init image_size variable */
 // RAMPZ = 0x01;
  image_size = pgm_read_word_near(SHADOW_META);
  meta_size += (uint8_t) pgm_read_word_near(SHADOW_META + 4);
  meta_size <<= 1; //Convert words to bytes

  /* Init hmac context with key (load 20 byte key temporary in buff) */
  load_key(buff,key_hmac);
  hmac_sha256_init(&ctx, buff, 256);

  /* Hash full app pages first */
  while(image_size >= PAGE_SIZE) {
    read_page(buff, offset);
    /* Hash full page, unroll loop */
    hmac_sha256_nextBlock(&ctx, buff);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES*2);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES*3);
    /* Book keeping */
    image_size -= PAGE_SIZE;
    offset += PAGE_SIZE;
  }

  /* Hash last (semi)page + metadata */
  read_page(buff, offset);
  read_page(buff + image_size, SHADOW_META);
  hmac_sha256_lastBlock(&ctx, buff, (image_size + meta_size)*8);
  memcpy_boot(digest, buff+image_size+meta_size, 32); //Backup digest from metadata page

  /* Finalize + compare */
  hmac_sha256_final(buff, &ctx);
  if(memcmp_boot(buff, digest, 32) != 0)
    return 0;

  return 1;
}

/****************************************************************************/
/*                        MICROVISOR CORE FUNCTIONS                         */
/****************************************************************************/

/* ALWAYS first disable global interrupts as first order of business in these
 * fucntions. Failing to do so could have untrusted interrupt handlers modify
 * the state and outcome of any of these trusted functions. */

/* Writes page contained in page_buf (256 bytes) to offset in deployment space
 * (0xFE00-0x1FC00) */

BOOTLOADER_SECTION void
load_image(uint8_t *page_buf, uint16_t offset) {
  uint8_t sreg;
  sreg = SREG;
  cli();

  /* Write page if it is within the allowable space */
  if(offset<SHADOW)
    write_page(page_buf, ((uint32_t) SHADOW) + offset);

  SREG = sreg;
  sei();
}

/* Verifies and activates an image from deployment app space to running app space.
 * When successful, this function will not return but perform a soft reset. In
 * case of failure, 0 (false) is returned */

BOOTLOADER_SECTION uint8_t
verify_activate_image() {
  uint8_t sreg;
  sreg = SREG;
  cli();

  if(!verify_shadow() || !verify_hmac())
    return 0;

  /* We passed all tests, activate new image and jump to it */
  switch_image();
  SREG = sreg;
  sei();
  goto *(0x0000);
}

/* Remote attestation */
BOOTLOADER_SECTION void 
remote_attestation(uint8_t *mac) {

  
//  uint8_t sreg;
//  sreg = SREG;
//  cli();

 hmac_sha256_ctx_t ctx;
 uint8_t buff[PAGE_SIZE];
 uint32_t offset;

  // Init hmac context with key (load 20 byte key temporary in buff)
 load_key(buff,key_hmac);
 
 hmac_sha256_init(&ctx, buff, 256);

  // Hash full image
  offset = 0x00;
  while(offset < MEM_END) {
    read_page(buff, offset);
    // Run block through HMAC, unroll loop
    hmac_sha256_nextBlock(&ctx, buff);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES*2);
    hmac_sha256_nextBlock(&ctx, buff + HMAC_SHA256_BLOCK_BYTES*3);
    // Increment counter
    offset += 0x100;
  }


  // Hash nonce
  hmac_sha256_lastBlock(&ctx, mac, 20*8); //20 byte nonce

  // Finalize
  hmac_sha256_final(mac, &ctx); 

  // SREG = sreg;

}


BOOTLOADER_SECTION void att_resp(uint8_t *msg_buf, uint8_t *result_msg, uint8_t mem_changed, uint8_t *metadata, uint8_t *prev_mem_state) {
  uint16_t ctr;
  uint64_t nonce[2];
  uint64_t att_resp_keyword = 0x6666666666666666;
  uint16_t self_id = 1000;
  uint8_t ver_mac[] = {0x02, 0x00, 0x00, 0x99, 0x99, 0x99};

  for(uint16_t i = 0; i < HASH_MAP_SIZE; i++) {
    metadata[i] = 0;
  }

  memcpy(&ctr, msg_buf + 22, 2);
  memcpy(nonce, msg_buf + 24, 16);

  uint8_t memory_state[32];
  if(!mem_changed) {
    memcpy(memory_state, prev_mem_state, 32);
    goto att_skip;
  }

  remote_attestation(memory_state);

att_skip:
  memcpy(result_msg, ver_mac, 6);
  memcpy(result_msg + 14, &self_id, 2);
  memcpy(result_msg + 16, &ctr, 2);
  memcpy(result_msg + 18, nonce, 16);
  memcpy(result_msg + 34, &att_resp_keyword, 8);
  memcpy(result_msg + 42, memory_state, 32);

  hmac_sha256_ctx_t ctx;
  uint8_t buff[PAGE_SIZE];
  uint32_t offset;

  // Init hmac context with key (load 20 byte key temporary in buff)
  load_key(buff,key_hmac);
 
  hmac_sha256_init(&ctx, buff, 256);
  hmac_sha256_nextBlock(&ctx, result_msg);
  hmac_sha256_lastBlock(&ctx, result_msg + SHA256_BLOCK_BYTES, 80);
  hmac_sha256_final(result_msg + 74, &ctx); 
}

BOOTLOADER_SECTION void status_update(uint8_t *msg_buf, uint8_t msg_length, uint8_t keyword, uint8_t *metadata) {

  if(keyword == 3) {
    for(uint8_t i = 0; i < HASH_MAP_SIZE; i++) {
      metadata[i] = 1;
    }
    return;
  }

  uint8_t valid_list_array_size = (msg_length - 22) / 8;
  // uint8_t valid_list_array_size = HASH_MAP_SIZE / 4;
  uint64_t *valid_list_ptr = (uint64_t*)(msg_buf + 22);

  if(keyword == 2) {
    for(uint64_t i = 0; i < valid_list_array_size; i++) {
      for(uint16_t j = 1; j < HASH_MAP_SIZE; j++) {
        if(valid_list_ptr[i] & (1 << (j - 1))) {
          metadata[j] = 1;
        }
      }
    }
    return;
  } 
  
  for(uint64_t i = 0; i < valid_list_array_size; i++) {
    for(uint16_t j = 1; j < HASH_MAP_SIZE; j++) {
      if(*valid_list_ptr & (1 << (j - 1))) {
        metadata[j] = 1;
      } else {
        metadata[j] &= 2;
      }
    }
  }

}

BOOTLOADER_SECTION int8_t parse_att_msg(const uint8_t *msg, uint8_t msg_length, uint8_t *result_msg, uint8_t mem_changed, uint8_t *metadata, uint8_t *prev_mem_state) {
  // uint8_t sreg;
  // sreg = SREG;
  // cli();

  int8_t retval = 0;

  uint64_t att_req_kwrd = 0x1111111111111111;
  uint64_t status_update_kwrd = 0x2222222222222222;
  uint64_t status_valid_kwrd = 0x3333333333333333;
  uint64_t status_final_kwrd = 0x4444444444444444;
  static const uint8_t verif_mac[] = {0x02, 0x00, 0x00, 0x99, 0x99, 0x99};

  uint8_t msg_buf[100] = {0};
  memcpy(msg_buf, msg, msg_length);

  for(uint8_t i = 6; i < 12; i++) {
    if (msg_buf[i] != verif_mac[i-6]) {
        retval = -1;
        goto end;
    }
  }

  uint64_t *kwrd_ptr = (uint64_t*)(msg_buf + 14);

  if(*kwrd_ptr == att_req_kwrd) {
    att_resp(msg_buf, result_msg, mem_changed, metadata, prev_mem_state);
    retval = 1;
    goto end;
  } else if(*kwrd_ptr == status_update_kwrd){
    status_update(msg_buf, msg_length, 2, metadata);
    retval = 2;
    goto end;
  } else if(*kwrd_ptr == status_valid_kwrd){
    status_update(NULL, 0, 3, metadata);
    retval = 3;
    goto end;
  } else if(*kwrd_ptr == status_final_kwrd){
    status_update(msg_buf, msg_length, 4, metadata);
    retval = 4;
    goto end;
  } else {
    retval = -2;
    goto end;
  }

end:
  // SREG = sreg;
  // sei();
  return retval;
}

BOOTLOADER_SECTION uint16_t hash_mac_address(unsigned char* mac) {
    unsigned int hash = 0;
    for (int i = 0; i < 6; i++) {
        hash = hash * 31 + mac[i];  // 31 is a small prime number
    }
    return hash % HASH_MAP_SIZE;
}

BOOTLOADER_SECTION int8_t device_auth(uint8_t *MAC, uint8_t *update_req_msg, uint8_t *metadata, uint16_t *prover_id_map) {
  // uint8_t sreg = SREG;
  // cli();

  int8_t retval = 0;

  uint64_t update_req_kwrd = 0x5555555555555555;
  uint16_t id = prover_id_map[hash_mac_address(MAC)];
  uint8_t meta = metadata[id];
  static const uint8_t verif_mac[] = {0x02, 0x00, 0x00, 0x99, 0x99, 0x99};

  if(meta & 1) {
    retval = 1;
    goto end;
  } else if(meta & 2) {
    goto end;
  } else {
    if(!(meta & 4)) {
      memcpy(update_req_msg, verif_mac, 6);
      memcpy(update_req_msg + 14, &update_req_kwrd, 8);
      meta &= 4;
    }
  }

end:
  // SREG = sreg;
  // sei();
  return retval;
}

BOOTLOADER_SECTION void map_init(uint16_t *prover_id_map) {

  uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

  for(uint16_t i = 0; i < HASH_MAP_SIZE; i++) {
    uint16_t id = hash_mac_address(mac);
    prover_id_map[id] = id;
    uint16_t *last_bytes_ptr = (uint16_t*)(mac +  4);
    *last_bytes_ptr++;
  }
}
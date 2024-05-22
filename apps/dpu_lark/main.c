#include <avr/io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <avr/interrupt.h>

#define BAUD 9600
#include <util/setbaud.h>

#include "microvisor.h"
#include "serial.h"

#define LOC_HASH_MAP_SIZE 32

volatile uint32_t timer1_overflows = 0;

void timer1_init() {
    TCCR1A = 0;          // Set entire TCCR1A register to 0
    TCCR1B = 0;          // Same for TCCR1B
    TCNT1  = 0;          // Initialize counter value to 0

    // Set CS10 bit for no prescaler (Timer1 clock = system clock)
    TCCR1B |= (1 << CS10);
}

void timer1_init2() {
    TCCR1A = 0;          // Set entire TCCR1A register to 0
    TCCR1B = 0;          // Same for TCCR1B
    TCNT1  = 0;          // Start counter from 0

    // Set up timer with prescaler = 64
    // TCCR1B |= (1 << CS11) | (1 << CS10);
    TCCR1B |= (1 << CS11);
    // Enable overflow interrupt
    TIMSK1 |= (1 << TOIE1);

    // Enable global interrupts
    sei();
}

ISR(TIMER1_OVF_vect) {
    timer1_overflows++;  // Increment the overflow counter
}

uint16_t read_timer1() {
    return TCNT1;  // Read the count value of Timer1
}

void uint32_to_string(uint32_t num, char* str) {
    char* p = str;
    if (num == 0) {
        *p++ = '0';
        *p = '\0';
        return;
    }

    // Fill the buffer backward
    while (num > 0) {
        *p++ = (num % 10) + '0';  // Get the last digit and convert to char
        num /= 10;  // Move to the next digit
    }
    *p = '\0';

    // Reverse the string
    char *p1 = str;
    char *p2 = p - 1;  // p is at '\0', so go back one character for the last valid char
    while (p1 < p2) {
        char tmp = *p1;
        *p1 = *p2;
        *p2 = tmp;
        p1++;
        p2--;
    }
}

// void uint64_to_string(uint64_t value, char* str) {
//     char* ptr = str;
//     char* ptr1 = str;
//     char tmp_char;
//     uint64_t tmp_value;

//     // Check for zero case
//     if (value == 0) {
//         *ptr++ = '0';
//     }

//     // Process individual digits
//     while (value) {
//         tmp_value = value;
//         value /= 10;
//         *ptr++ = '0' + (tmp_value - value * 10);
//     }

//     // Null terminate string
//     *ptr-- = '\0';

//     // Reverse the string
//     while (ptr1 < ptr) {
//         tmp_char = *ptr;
//         *ptr--= *ptr1;
//         *ptr1++ = tmp_char;
//     }
// }

void print_uint32(uint32_t num) {
    char buffer[11];  // Maximum 10 digits plus null terminator
    uint32_to_string(num, buffer);
    uart_puts(buffer);
    uart_putchar('\n');
}

void print_hex(uint8_t num) {
    const char hexDigits[] = "0123456789ABCDEF";
    uart_putchar(hexDigits[num >> 4]);   // High nibble
    uart_putchar(hexDigits[num & 0x0F]); // Low nibble
}

void print_buffer_hex(const uint8_t *buffer, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && i % 16 == 0) {   // New line every 16 bytes for readability
            uart_puts("\r\n");
        }
        print_hex(buffer[i]);
        uart_puts(" ");   // Space between bytes
    }
    uart_puts("\r\n");   // New line at the end
}

void uart_print_int8(int8_t num) {
    char buffer[5]; // Enough space for '-128\0'
    itoa(num, buffer, 10); // Convert the integer to a string in base 10
    uart_puts(buffer); // Send the string over UART
    uart_putchar('\n');
}

uint32_t calculate_microseconds(uint16_t start_time, uint16_t end_time, uint32_t overflows, uint8_t prescaler) {
    uint32_t elapsed_ticks;
    float multiplier;

    // Calculate the number of ticks based on whether overflow occurred
    if (end_time >= start_time) {
        elapsed_ticks = end_time - start_time; // Normal condition
    } else {
        elapsed_ticks = (0xFFFF - start_time) + end_time + 1; // Handle possible overflow
    }


    uint32_t total_ticks = overflows * 0x10000 + elapsed_ticks;

    // Set the multiplier based on the prescaler
    switch (prescaler) {
        case 1: // No prescaler
            multiplier = 0.125; // Each tick is 0.125 microseconds
            break;
        case 8: // Prescaler of 8
            multiplier = 1; // Each tick is 1 microsecond
            break;
        default:
            return 0; // Unsupported prescaler value
    }

    // Calculate elapsed time in microseconds
    return (uint32_t)(total_ticks * multiplier);
}

int main(void) {

  // uint32_t mem_size = 8 * 1024; //4 kb
  // volatile uint8_t *size_ctrl = (uint8_t*)malloc(mem_size);
  // for(uint32_t i = 0; i < mem_size; i++) {
  //   size_ctrl[i] = 1;
  // }

  uint8_t metadata[LOC_HASH_MAP_SIZE] = {0};
  uint16_t prover_id_map[LOC_HASH_MAP_SIZE] = {0};
  uint8_t prev_mem_state[32] = {0};

  map_init(prover_id_map);
  uart_init();
  uart_puts("Starting experiments for prover count: ");
  print_uint32(LOC_HASH_MAP_SIZE);
  uart_puts("Initialized hash map\n");

  uint16_t start_time;
  uint16_t end_time;
  uint32_t elapsed_time;
  uint32_t elapsed_timer_over;
  char llu_buffer[21];

  uint8_t verif_mac[6] = {0x02, 0x00, 0x00, 0x99, 0x99, 0x99};
  uint8_t self_mac[6] = {0x02, 0x00, 0x00, 0xbb, 0xbb, 0xbb};
  uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  uint8_t msg_length;
  volatile uint8_t ver_msg_buff[108];
  volatile uint8_t prv_msg_buff[108];
  uint16_t ctr = 3;
  uint64_t nonce[2] = {
    0x0123456789abcdef,
    0xfedcba9876543210
  };
  uint64_t att_req_kwrd = 0x1111111111111111;
  uint64_t status_update_kwrd = 0x2222222222222222;
  uint64_t status_valid_kwrd = 0x3333333333333333;
  uint64_t status_final_kwrd = 0x4444444444444444;

  uint8_t valid_list_array_length = LOC_HASH_MAP_SIZE / 64;
  uint64_t valid_list[valid_list_array_length];
  for (uint8_t i = 0; i < valid_list_array_length; i++) {
    valid_list[i] = 0xfffffffffffffffe;
  }
  int8_t retval = 0;
  memcpy(ver_msg_buff, broadcast_mac, 6);
  memcpy(ver_msg_buff + 6, verif_mac, 6);
  memcpy(prv_msg_buff + 6, self_mac, 6);

/*_____________________status_valid____________________________*/
  // memcpy(ver_msg_buff + 14, &status_valid_kwrd, 8);

  // memcpy(prv_msg_buff + 6, self_mac, 6);

  // print_buffer_hex(ver_msg_buff, 22);

  // uart_puts("-------------------------------------\n");
  // uart_puts("Starting status_valid trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = parse_att_msg(ver_msg_buff, 22, NULL, 1, metadata, prev_mem_state);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);
  // uart_puts("-------------------------------------\n");

/*_____________________sttaus_final____________________________*/
  // memcpy(ver_msg_buff + 14, &status_final_kwrd, 8);
  // memcpy(ver_msg_buff + 22, valid_list, valid_list_array_length * 8);
  // msg_length = valid_list_array_length * 8 + 22;
  // print_buffer_hex(ver_msg_buff, msg_length);
  // uart_puts("Starting status_final trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = parse_att_msg(ver_msg_buff, msg_length, NULL, 1, metadata, prev_mem_state);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);
  // uart_puts("-------------------------------------\n");

 /*_____________________status_update____________________________*/
  // memcpy(ver_msg_buff + 14, &status_update_kwrd, 8);
  // memcpy(ver_msg_buff + 22, valid_list, valid_list_array_length * 8);
  // msg_length = valid_list_array_length * 8 + 22;
  // print_buffer_hex(ver_msg_buff, msg_length);
  // uart_puts("Starting status_update trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = parse_att_msg(ver_msg_buff, msg_length, NULL, 1, metadata, prev_mem_state);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);
  // uart_puts("-------------------------------------\n");

// /*_____________________Att_resp____________________________*/
  memcpy(ver_msg_buff + 14, &att_req_kwrd, 8);
  memcpy(ver_msg_buff + 22, &ctr, 2);
  memcpy(ver_msg_buff + 24, nonce, 16);
  msg_length = 108;
  print_buffer_hex(ver_msg_buff, msg_length);
  uart_puts("Starting att_resp trial\n");
  cli();
  timer1_overflows = 0;
  timer1_init2();
  sei();
  start_time = read_timer1();

  retval = parse_att_msg(ver_msg_buff, msg_length, prv_msg_buff, 1, metadata, prev_mem_state);

  cli();
  end_time = read_timer1();
  elapsed_timer_over = timer1_overflows;
  sei();
  
  elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  uart_puts("Finished trial. Time: ");
  print_uint32(elapsed_time);
  uart_puts("return value: ");
  uart_print_int8(retval);
  print_buffer_hex(prv_msg_buff, 106);
  uart_puts("-------------------------------------\n");


/*_____________________Att_resp_imrpoved____________________________*/
  // memcpy(ver_msg_buff + 14, &att_req_kwrd, 8);
  // memcpy(ver_msg_buff + 22, &ctr, 2);
  // memcpy(ver_msg_buff + 24, nonce, 16);
  // msg_length = 40;
  // print_buffer_hex(ver_msg_buff, msg_length);
  // uart_puts("Starting att_resp_im trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = parse_att_msg(ver_msg_buff, msg_length, prv_msg_buff, 0, metadata, prev_mem_state);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);
  // print_buffer_hex(prv_msg_buff, 106);
  // uart_puts("-------------------------------------\n");

  /*__________________device_auth____________________________*/
  // memcpy(ver_msg_buff + 14, &status_valid_kwrd, 8);
  // parse_att_msg(ver_msg_buff, msg_length, prv_msg_buff, 0, metadata, prev_mem_state);
  // uint8_t remote_mac[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  // uart_puts("Starting device_auth trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = device_auth(remote_mac, prv_msg_buff, metadata, prover_id_map);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);
  // print_buffer_hex(prv_msg_buff, 22);



  /*__________________device_auth_valid____________________________*/
  // memcpy(ver_msg_buff + 14, &status_valid_kwrd, 8);
  // parse_att_msg(ver_msg_buff, msg_length, prv_msg_buff, 0, metadata, prev_mem_state);

  // uart_puts("Starting device_auth_valid trial\n");
  // cli();
  // timer1_overflows = 0;
  // timer1_init2();
  // sei();
  // start_time = read_timer1();

  // retval = device_auth(remote_mac, prv_msg_buff, metadata, prover_id_map);

  // cli();
  // end_time = read_timer1();
  // elapsed_timer_over = timer1_overflows;
  // sei();
  
  // elapsed_time = calculate_microseconds(start_time, end_time, elapsed_timer_over, 8);
  // uart_puts("Finished trial. Time: ");
  // print_uint32(elapsed_time);
  // uart_puts("return value: ");
  // uart_print_int8(retval);

  // uart_puts("Finished all trials\n");
}

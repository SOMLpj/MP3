#include "FreeRTOS.h"
#include "cli_handlers.h"
#include "ff.h"
#include "gpio.h"
#include "queue.h"
#include "sj2_cli.h"
#include "ssp2.h"
#include "task.h"
#include <acceleration.h>
#include <clock.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

QueueHandle_t songData_Queue;
QueueHandle_t songName_Queue;

typedef struct {
  char song_name[64];
} songName_s;

typedef struct {
  uint8_t song_data[512];
} songData_s;

void mp3_decoder_SCI_cs(void) { // chip select
  LPC_GPIO2->CLR = (1 << 2);
}

void mp3_decoder_SCI_ds(void) { // deselect chip
  LPC_GPIO2->SET = (1 << 2);
}
void mp3_decoder_SDI_cs(void) { // chip select
  LPC_GPIO2->CLR = (1 << 5);
}

void mp3_decoder_SDI_ds(void) { // deselect chip
  LPC_GPIO2->SET = (1 << 5);
}
void reset_decoder() {
  LPC_GPIO2->CLR = (1 << 9); // XRST low
}
void restart_decoder() {
  LPC_GPIO2->SET = (1 << 9); // XRST high
}

void pin_configuration_for_decoder() {
  gpio_s CS = gpio__construct_with_function(GPIO__PORT_2, 2, GPIO__FUNCITON_0_IO_PIN);    // chip select for sci
  gpio_s DCS = gpio__construct_with_function(GPIO__PORT_2, 5, GPIO__FUNCITON_0_IO_PIN);   // chip select for sdi
  gpio_s DREQ = gpio__construct_with_function(GPIO__PORT_2, 7, GPIO__FUNCITON_0_IO_PIN);  // DREQ
  gpio_s RESET = gpio__construct_with_function(GPIO__PORT_2, 9, GPIO__FUNCITON_0_IO_PIN); // RESET
  gpio__set_as_input(DREQ);                              // set DREQ pin from decoder as input for sjtwo
  gpio__set_as_output(CS);                               // Set chip select as output for sci
  gpio__set_as_output(DCS);                              // Set chip select as output for sdi
  gpio__set_as_output(RESET);                            // Set chip select as output for sdi
  gpio__construct_with_function(1, 0, GPIO__FUNCTION_4); // sck2
  gpio__construct_with_function(1, 1, GPIO__FUNCTION_4); // MOSI
  gpio__construct_with_function(1, 4, GPIO__FUNCTION_4); // MISO
}
bool decoder_is_requesting_data() {
  gpio_s dreq_pin;
  dreq_pin.port_number = 2;
  dreq_pin.pin_number = 7;

  return gpio__get(dreq_pin);
}
void write_16bits_on_decoder(uint8_t address, uint16_t data) {
  while (!decoder_is_requesting_data()) {
    ; // wait
  }
  mp3_decoder_SCI_cs();
  ssp2__exchange_byte(0x2); // Opcode for write
  ssp2__exchange_byte(address);
  ssp2__exchange_byte(data >> 8);
  ssp2__exchange_byte(data);
  while (!decoder_is_requesting_data()) {
    ; // wait
  }

  mp3_decoder_SCI_ds();
}
void decoder_init() {
  mp3_decoder_SCI_ds();
  //^here we are pulling up the XCS pin so that we are not beignning any SPI communication while initializing
  mp3_decoder_SDI_ds();
  //^here we are pulling up the XDCS pin so that we are not beignning any SPI communication while initializing
  reset_decoder();
  //^turned of the decoder
  delay__ms(100);
  restart_decoder();
  //^after 100ms we will restart the decoder (basically pulling the XRST high)
  delay__ms(10);
  write_16bits_on_decoder(0x00, 0x4800); // SM_SDINEW =1
  //^this is questionable operations I was testing different combinations of SCI mode and found out that this is reqd
  // for native recommended mode of VS10xx chip (but we can comment it out in case you feel it is redundant)
  delay__ms(10);
  write_16bits_on_decoder(0x0B, 0x0000); // setting volume to MAX
  delay__ms(10);
  write_16bits_on_decoder(0x03, 0x6000); // clock multiplier
  //^as I am sending 1 MHZ on main() I was thinking of setting this SCI register such that the clock gets multiplied by
  // 3 internally inside the vs10xx chip (again it was for test only not sure if we need this)
  delay__ms(10);
}

void mp3_reader_task(void *p) {
  songName_s song_to_play;
  FIL file; // object file
  FRESULT is_file_open;
  songData_s bytes;
  UINT br;

  while (1) {
    xQueueReceive(songName_Queue, song_to_play.song_name, portMAX_DELAY);
    is_file_open = f_open(&file, song_to_play.song_name, FA_READ);
    if (FR_OK == is_file_open) {
      while (!(f_eof(&file))) {

        if (FR_OK == f_read(&file, bytes.song_data, sizeof(bytes.song_data), &br)) {

          xQueueSend(songData_Queue, &bytes, portMAX_DELAY);
        }
      }
      f_close(&file);
    } else {
      printf("Error: Failed to open.\n");
    }
  }
}

static void transfer_data_block(songData_s *mp3_playback_buffer) {
  for (int index = 0; index < 512; index++) {

    while (!decoder_is_requesting_data()) {
      ;
    }
    mp3_decoder_SDI_cs();
    ssp2__exchange_byte(mp3_playback_buffer->song_data[index]);
    mp3_decoder_SDI_ds();
  }
}

static void mp3_player_task(void *parameter) {
  songData_s mp3_playback_buffer;

  while (1) {
    xQueueReceive(songData_Queue, &mp3_playback_buffer, portMAX_DELAY);
    transfer_data_block(&mp3_playback_buffer);
  }
}

int main(void) {
  sj2_cli__init();
  pin_configuration_for_decoder();
  uint32_t clock_for_ssp_in_KHz = 1000;
  ssp2__initialize(clock_for_ssp_in_KHz);
  decoder_init();
  songName_Queue = xQueueCreate(1, sizeof(songName_s));
  songData_Queue = xQueueCreate(5, sizeof(songData_s));

  xTaskCreate(mp3_reader_task, "Reader", (5 * 1024) / sizeof(void *), NULL, PRIORITY_LOW, NULL);
  xTaskCreate(mp3_player_task, "Player", (5 * 1024) / sizeof(void *), NULL, PRIORITY_HIGH, NULL);
  vTaskStartScheduler();
  return 0;
}

// // MP3 PROJECT********************************************************************************************
// #include "FreeRTOS.h"
// #include "cli_handlers.h"
// #include "ff.h"
// #include "gpio.h"
// #include "queue.h"
// #include "sj2_cli.h"
// #include "task.h"
// #include <acceleration.h>
// #include <clock.h>
// #include <stdbool.h>
// #include <stdio.h>
// #include <string.h>
// QueueHandle_t songData_Queue;
// QueueHandle_t songName_Queue;

// FATFS FatFs;

// typedef struct {
//   char song_name[64];
// } songName_s;

// typedef struct {
//   uint8_t song_data[512];
// } songData_s;

// // CLI needs access to the QueueHandle_t where you can queue the song name
// // One way to do this is to declare 'QueueHandle_t' in main() that is NOT static
// // and then extern it here

// app_cli_status_e cli__mp3_play(app_cli__argument_t argument, sl_string_s user_input_minus_command_name,
//                                app_cli__print_string_function cli_output) {
//   // user_input_minus_command_name is actually a 'char *' pointer type
//   // We tell the Queue to copy 32 bytes of songname from this location
//   xQueueSend(songName_Queue, &user_input_minus_command_name, 0);
//   printf("Sent %s over to the Q_songname\n", user_input_minus_command_name);
//   vTaskDelay(100);
//   return APP_CLI_STATUS__SUCCESS;
// }

// void mp3_reader_task(void *p) {
//   songName_s song_to_play;
//   FIL file; // object file
//   FRESULT is_file_open;
//   char bytes[512];

//   while (1) {
//     xQueueReceive(songName_Queue, &song_to_play.song_name[0], portMAX_DELAY);
//     printf("Received song to play: %s\n", song_to_play);
//     // Open File:
//     f_mount(&FatFs, "", 0);
//     is_file_open = f_open(&file, song_to_play.song_name, "rb");
//     // printf("Song is in SD Card\n");
//     if (FR_OK == is_file_open) {
//       while (f_eof(&file)) {
//         // Read from file
//         f_read(&file, bytes, sizeof(bytes), 512);
//         xQueueSend(songData_Queue, &bytes[0], 0);
//         vTaskDelay(100);
//       }
//     } else {
//       printf("Error: Failed to open.\n");
//       vTaskDelay(1000);
//     }
//     printf("CLOSE\n");
//     f_close(&file);
//   }
// }

// int main(void) {
//   sj2_cli__init();
//   acceleration__init();
//   songName_Queue = xQueueCreate(1, sizeof(songName_s));
//   songData_Queue = xQueueCreate(1, sizeof(songData_s));

//   xTaskCreate(mp3_reader_task, "Reader", 12000 / sizeof(void *), NULL, PRIORITY_LOW, NULL);
//   vTaskStartScheduler();
//   return 0;
// }

// //*******************************************************************************************************
// // #include <stdio.h>
// // #include <stdlib.h>

// // #include "FreeRTOS.h"
// // #include "adc.h"
// // #include "board_io.h"
// // #include "common_macros.h"
// // #include "gpio.h"
// // #include "gpio_isr.h"
// // #include "i2c_slave_init.h"
// // #include "lpc40xx.h"
// // #include "lpc_peripherals.h"
// // #include "periodic_scheduler.h"
// // #include "pwm1.h"
// // #include "queue.h"
// // #include "semphr.h"
// // #include "sj2_cli.h"
// // #include "ssp2.h"
// // #include "ssp2_lab.h"
// // #include "task.h"
// // #include "uart_lab.h"

// //*******************************************************************************************************
// // int main(void) {
// //   // i2c2__slave_init(0x86);
// //   sj2_cli__init();
// //   // acceleration__init();

// //   puts("Starting RTOS");
// //   vTaskStartScheduler(); // This function never returns unless RTOS scheduler runs out of memory and fails

// //   return 0;
// // }

// //*********************************************************************************************************
// // #include <stdio.h>
// // #include <stdlib.h>

// // #include "FreeRTOS.h"
// // #include "adc.h"
// // #include "board_io.h"
// // #include "common_macros.h"
// // #include "gpio.h"
// // #include "gpio_isr.h"
// // #include "lpc40xx.h"
// // #include "lpc_peripherals.h"
// // #include "periodic_scheduler.h"
// // #include "pwm1.h"
// // #include "queue.h"
// // #include "semphr.h"
// // #include "sj2_cli.h"
// // #include "ssp2.h"
// // #include "ssp2_lab.h"
// // #include "task.h"
// // #include "uart_lab.h"

// // static QueueHandle_t adc_to_pwm_task_queue;
// // static QueueHandle_t switch_queue;
// // static SemaphoreHandle_t switch_press_indication;
// // static SemaphoreHandle_t switch_pressed_signal;
// // static SemaphoreHandle_t spi_bus_mutex;
// // // static QueueHandle_t q;

// // void pin30_isr(void) { fprintf(stderr, "PIN 30\n"); }
// // void pin29_isr(void) { fprintf(stderr, "PIN 29\n"); }

// // typedef enum { x_den = 0, x_en = 1 } val_e;

// // typedef enum { switch__off = 0, switch__on = 1 } switch_e;
// // typedef struct {
// //   /* First get gpio0 driver to work only, and if you finish it
// //    * you can do the extra credit to also make it work for other Ports
// //    */
// //   // uint8_t port;

// //   uint8_t pin;
// // } port_pin_s;

// // // TODO: Implement Adesto flash memory CS signal as a GPIO driver

// // // TODO: Study the Adesto flash 'Manufacturer and Device ID' section
// // typedef struct {
// //   uint8_t manufacturer_id;
// //   uint8_t device_id_1;
// //   uint8_t device_id_2;
// //   uint8_t extended_device_id;
// // } adesto_flash_id_s;

// // switch_e get_switch_input_from_switch0() {
// //   LPC_GPIO0->DIR |= ~(1 << 29);   // SET INPUT
// //   if (LPC_GPIO0->PIN & (1 << 29)) // IF BUTTON PRESSED
// //     return switch__on;
// //   else
// //     return switch__off;
// // }

// // void main() {}

// // // TODO: Create this task at PRIORITY_LOW
// // // void producer(void *p) {
// // //   while (1) {
// // //     // uint8_t x = 1;
// // //     // This xQueueSend() will internally switch context to "consumer" task because it is higher priority than
// this
// // //     // "producer" task Then, when the consumer task sleeps, we will resume out of xQueueSend()and go over to
// the
// // next
// // //     // line

// // //     // TODO: Get some input value from your board
// // //     // const switch_e switch_value = get_switch_input_from_switch0();

// // //     // TODO: Print a message before xQueueSend()
// // //     printf("MESSAGE BEFORE xQueueSend()\n");
// // //     // Note: Use printf() and not fprintf(stderr, ...) because stderr is a polling printf
// // //     xQueueSend(switch_queue, x_en, 0); // ONLY BLOCK IF FULL
// // //     printf("MESSAGE AFTER xQueueSend()\n");
// // //     // TODO: Print a message after xQueueSend()
// // //     printf("\n%d\n", x);

// // //     vTaskDelay(1000);
// // //   }
// // // }

// // // POLLING --> 0
// // // INTERRUPT --> PORTMAX_DELAY

// // // TODO: Create this task at PRIORITY_HIGH
// // // void consumer(void *p) {
// // //   uint8_t y;
// // //   while (1) {
// // //     // TODO: Print a message before xQueueReceive()
// // //     printf("MESSAGE BEFORE xQueueReceive()\n");
// // //     xQueueReceive(switch_queue, &y, portMAX_DELAY); // put to sleep until somethign in q
// // //     // TODO: Print a message after xQueueReceive()
// // //     printf("MESSAGE AFTER xQueueReceive()\n");
// // //     // printf("I AM PRINTING SHORT");
// // //   }
// // // }

// // // void led_task(void *task_parameter) {
// // //   // Type-cast the paramter that was passed from xTaskCreate()
// // //   const port_pin_s *led = (port_pin_s *)(task_parameter);
// // //   uint8_t led_pin = led->pin;

// // //   LPC_GPIO1->PIN &= ~(111 << 0);
// // //   LPC_GPIO1->DIR |= (1 < led_pin);
// // //   while (true) {
// // //     if (xSemaphoreTake(switch_press_indication, 1000)) {
// // //       LPC_GPIO1->PIN &= ~(1 << led_pin);
// // //     } else {
// // //       LPC_GPIO1->PIN |= (1 << led_pin);
// // //       vTaskDelay(500);
// // //     }
// // //   }
// // // }

// // void led_task(void *task_parameter) {
// //   // Type-cast the paramter that was passed from xTaskCreate()
// //   const port_pin_s *led = (port_pin_s *)(task_parameter);
// //   uint8_t led_pin = led->pin;

// //   LPC_GPIO1->PIN &= ~(111 << 0);
// //   LPC_GPIO1->DIR |= (1 < led_pin);
// //   while (true) {
// //     LPC_GPIO1->PIN &= ~(1 << led_pin);
// //     vTaskDelay(500);
// //     LPC_GPIO1->PIN |= (1 << led_pin);
// //     vTaskDelay(500);
// //   }
// // }

// // void switch_task(void *task_parameter) {
// //   port_pin_s *switch_b = (port_pin_s *)task_parameter;
// //   uint8_t switch_pin = switch_b->pin;
// //   LPC_GPIO1->DIR &= ~(1 < switch_pin); // INPUT

// //   while (true) {
// //     // TODO: If switch pressed, set the binary semaphore
// //     if (LPC_GPIO1->PIN & (1 << switch_pin)) {
// //       // printf("HI");
// //       fprintf(stderr, "SWITCH PRESS\n");
// //       xSemaphoreGive(switch_press_indication);
// //     }
// //     fprintf(stderr, "NO SWITCH PRESS\n");

// //     // Task should always sleep otherwise they will use 100% CPU
// //     // This task sleep also helps avoid spurious semaphore give during switch debeounce
// //     vTaskDelay(100);
// //   }
// // }

// // // void gpio_interrupt(void) {
// // //   fprintf(stderr, "ISR\n");
// // //   LPC_GPIOINT->IO0IntClr |= (1 << 29);
// // //   // a) Clear Port0/2 interrupt using CLR0 or CLR2 registers
// // //   // b) Use fprintf(stderr) or blink and LED here to test your ISR
// // // }

// // void sleep_on_sem_task(void *p) {
// //   LPC_GPIO1->DIR |= (1 << 26);
// //   while (xSemaphoreTake(switch_pressed_signal, portMAX_DELAY)) {
// //     fprintf(stderr, "TAKEN\n");
// //   }

// //   // while (1) {
// //   //   LPC_GPIO1->PIN |= (1 << 26);
// //   //   delay__ms(500);
// //   //   LPC_GPIO1->PIN &= ~(1 << 26);
// //   //   delay__ms(500);
// //   // }
// // }

// // void gpio_interrupt(void) {
// //   LPC_GPIOINT->IO0IntClr |= (1 << 29);
// //   fprintf(stderr, "ISR\n");
// //   xSemaphoreGiveFromISR(switch_pressed_signal, NULL);
// // }

// // void config_interrupt(void) {
// //   LPC_GPIOINT->IO0IntEnR |= (1 << 29);
// //   LPC_GPIO1->DIR &= ~(1 << 29);
// // }

// // void pwm_task(void *p) {
// //   // while (1) {
// //   //   fprintf(stderr, "PWM\n");
// //   //   vTaskDelay(100);
// //   // }
// //   pwm1__init_single_edge(1000);

// //   // Locate a GPIO pin that a PWM channel will control
// //   // NOTE You can use gpio__construct_with_function() API from gpio.h
// //   // TODO Write this function yourself

// //   LPC_IOCON->P2_0 &= ~0b001; // ASSIGN A FUNCTION -- PWM1
// //   LPC_IOCON->P2_0 |= 0b001;  // ASSIGN A FUNCTION -- PWM1

// //   // We only need to set PWM configuration once, and the HW will drive
// //   // the GPIO at 1000Hz, and control set its duty cycle to 50%
// //   // pwm1__set_duty_cycle(PWM1__2_0, 1);

// //   // Continue to vary the duty cycle in the loop
// //   uint8_t percent = 0;
// //   // while (1) {
// //   //   pwm1__set_duty_cycle(PWM1__2_0, percent);
// //   //   fprintf(stderr, "%d\n", percent);
// //   //   if (++percent > 100) {
// //   //     percent = 0;
// //   //   }
// //   //   vTaskDelay(100);
// //   // }
// //   uint16_t adc_reading;
// //   uint16_t copy;
// //   float voltage;

// //   while (1) {
// //     // Implement code to receive potentiometer value from queue
// //     if (xQueueReceive(adc_to_pwm_task_queue, &adc_reading, 100)) {
// //       fprintf(stderr, "RECEIVED FROM QUEUE %i\n", adc_reading);
// //     }
// //     copy = ((float)adc_reading / 4096 * 100) - 1;
// //     fprintf(stderr, "%f\n", copy);
// //     pwm1__set_duty_cycle(PWM1__2_0, copy);
// //     printf("%d \n", adc_reading);
// //     voltage = (float)adc_reading / 4096.0 * 3.3;
// //     fprintf(stderr, "MR0: %d \n", LPC_PWM1->MR0);
// //     fprintf(stderr, "MR1: %d \n", LPC_PWM1->MR1);
// //     fprintf(stderr, "TC: %d \n", LPC_PWM1->TC);

// //     // We do not need task delay because our queue API will put task to sleep when there is no data in the queue
// //     // vTaskDelay(10000);
// //   }
// // }

// // void adc_task(void *p) {
// //   const uint16_t adc_value = 1;
// //   adc__initialize();

// //   // TODO This is the function you need to add to adc.h
// //   // You can configure burst mode for just the channel you are using
// //   adc__enable_burst_mode();

// //   // Configure a pin, such as P1.31 with FUNC 011 to route this pin as ADC channel 5
// //   // You can use gpio__construct_with_function() API from gpio.h
// //   LPC_IOCON->P1_31 &= ~0b011;
// //   LPC_IOCON->P1_31 |= 0b011;
// //   LPC_IOCON->P1_31 &= ~(1 << 7);

// //   while (1) {
// //     // Get the ADC reading using a new routine you created to read an ADC burst reading
// //     // TODO: You need to write the implementation of this function
// //     const uint16_t adc_value = adc__get_channel_reading_with_burst_mode(ADC__CHANNEL_5);
// //     fprintf(stderr, "%i\n", adc_value);
// //     fprintf(stderr, "TO QUEUE\n");
// //     xQueueSend(adc_to_pwm_task_queue, &adc_value, 0);

// //     vTaskDelay(100);
// //   }

// //   // Note that this 'adc_reading' is not the same variable as the one from adc_task
// // }

// // void adesto_cs(void) {
// //   LPC_GPIO1->DIR |= (1 << 10);  // OUTPUT
// //   LPC_GPIO1->PIN &= ~(1 << 10); // SELECT
// // }

// // void adesto_ds(void) {
// //   LPC_GPIO1->PIN |= (1 << 10); // DESELECT
// // }
// // // TODO: Implement the code to read Adesto flash memory signature
// // // TODO: Create struct of type 'adesto_flash_id_s' and return it
// // adesto_flash_id_s adesto_read_signature(void) {
// //   adesto_flash_id_s data;

// //   adesto_cs();
// //   {
// //     // Send opcode and read bytes
// //     // TODO: Populate members of the 'adesto_flash_id_s' struct
// //     ssp2_exchange_byte(0x9F);
// //     data.manufacturer_id = ssp2_exchange_byte(0x00);
// //     data.device_id_1 = ssp2_exchange_byte(0x0);
// //     data.device_id_2 = ssp2_exchange_byte(0x0);
// //   }
// //   adesto_ds();

// //   return data;
// // }

// // void spi_task(void *p) {
// //   const uint32_t spi_clock_mhz = 24;
// //   ssp2__init(spi_clock_mhz);

// //   // From the LPC schematics pdf, find the pin numbers connected to flash memory
// //   // Read table 84 from LPC User Manual and configure PIN functions for SPI2 pins
// //   // You can use gpio__construct_with_function() API from gpio.h
// //   //
// //   // Note: Configure only SCK2, MOSI2, MISO2.

// //   gpio__construct_with_function(GPIO__PORT_1, 4, GPIO__FUNCTION_4);
// //   gpio__construct_with_function(GPIO__PORT_1, 1, GPIO__FUNCTION_4);
// //   gpio__construct_with_function(GPIO__PORT_1, 0, GPIO__FUNCTION_4);

// //   while (1) {
// //     adesto_flash_id_s id = adesto_read_signature();
// //     // TODO: printf the members of the 'adesto_flash_id_s' struct
// //     fprintf(stderr, "%x\n", id.manufacturer_id);
// //     fprintf(stderr, "%x\n", id.device_id_1);
// //     fprintf(stderr, "%x\n", id.device_id_2);
// //     vTaskDelay(500);
// //   }
// // }

// // void spi_id_verification_task(void *p) {
// //   while (1) {
// //     if (xSemaphoreTake(spi_bus_mutex, portMAX_DELAY)) {
// //       const adesto_flash_id_s id = adesto_read_signature();
// //       fprintf(stderr, "MAN ID: %x\n", id.manufacturer_id);
// //       if (0x1F != id.manufacturer_id) {
// //         fprintf(stderr, "Manufacturer ID read failure\n");
// //         vTaskSuspend(NULL); // Kill this task
// //       }
// //       xSemaphoreGive(spi_bus_mutex);
// //       vTaskDelay(100);
// //     }
// //   }
// // }

// // void uart_read_task(void *p) {
// //   char c = 'A';
// //   while (1) {
// //     uart_lab__polled_get(UART_2, &c);
// //     fprintf(stderr, "RECEIVED: %c\n", c);
// //     vTaskDelay(500);
// //   }
// // }

// // void uart_write_task(void *p) {
// //   char c = 'A';
// //   while (1) {
// //     uart_lab__polled_put(UART_2, c);
// //     fprintf(stderr, "WRITING: %c\n", c);
// //     vTaskDelay(500);
// //   }
// // }

// // /*

// // // TODO: Create this task at PRIORITY_LOW
// // void producer(void *p) {
// //   while (1) {
// //     // This xQueueSend() will internally switch context to "consumer" task because it is higher priority than
// this
// //     // "producer" task Then, when the consumer task sleeps, we will resume out of xQueueSend()and go over to the
// next
// //     // line

// //     // TODO: Get some input value from your board
// //     const switch_e switch_value = get_switch_input_from_switch0();

// //     // TODO: Print a message before xQueueSend()
// //     printf("MESSAGE BEFORE xQueueSend()\n");
// //     // Note: Use printf() and not fprintf(stderr, ...) because stderr is a polling printf
// //     xQueueSend(switch_queue, &switch_value, 0); // ONLY BLOCK IF FULL
// //     printf("MESSAGE AFTER xQueueSend()\n");
// //     // TODO: Print a message after xQueueSend()
// //     printf("\n%d\n", switch_value);

// //     vTaskDelay(1000);
// //   }
// // }

// // // POLLING --> 0
// // // INTERRUPT --> PORTMAX_DELAY

// // // TODO: Create this task at PRIORITY_HIGH
// // void consumer(void *p) {
// //   switch_e switch_value;
// //   while (1) {
// //     // TODO: Print a message before xQueueReceive()
// //     printf("MESSAGE BEFORE xQueueReceive()\n");
// //     xQueueReceive(switch_queue, &switch_value, portMAX_DELAY); // put to sleep until somethign in q
// //     // TODO: Print a message after xQueueReceive()
// //     printf("MESSAGE AFTER xQueueReceive()\n");
// //     // printf("I AM PRINTING SHORT");
// //   }
// // }
// // */
// // // void producer_midterm(void *p) {
// // //   uint8_t x = 1;
// // //   // while (1) {
// // //   //   printf("SENDING");
// // //   //   xQueueSend(q, &x, 0);
// // //   // }
// // //   printf("MESSAGE BEFORE xQueueSend()\n");
// // //   // Note: Use printf() and not fprintf(stderr, ...) because stderr is a polling printf
// // //   xQueueSend(q, &x, 0);
// // //   printf("MESSAGE AFTER xQueueSend()\n");
// // //   // TODO: Print a message after xQueueSend()
// // //   printf("\n%d\n", x);

// // //   vTaskDelay(1000);
// // // }

// // // void consumer_midterm(void *p) {
// // //   // while (1) {
// // //   //   xQueueReceive(q, &y, portMAX_DELAY);
// // //   //   printf("Received %d\n", y);
// // //   // }
// // //   while (1) {
// // //     // TODO: Print a message before xQueueReceive()
// // //     printf("MESSAGE BEFORE xQueueReceive()\n");
// // //     xQueueReceive(1, &y, portMAX_DELAY);
// // //     // TODO: Print a message after xQueueReceive()
// // //     printf("MESSAGE AFTER xQueueReceive()\n");
// // //     // printf("I AM PRINTING SHORT");
// // //   }
// // // }

// // // void main(void) {
// // //   // sj2_cli__init();
// // //   // switch_press_indication = xSemaphoreCreateBinary();

// // //   // // static port_pin_s led0 = {26};
// // //   // // static port_pin_s led1 = {15};
// // //   // static port_pin_s switch_button = {15};

// // //   // xTaskCreate(switch_task, "SWITCH", 2048, &switch_button, 1, NULL);
// // //   // xTaskCreate(led_task, "LED", 2048, &led1, 1, NULL);

// // //   // xTaskCreate(led_task, "LED", 2048, &led0, 1, NULL);

// // //   xTaskCreate(producer, "producer", 1000, NULL, 1, NULL);
// // //   xTaskCreate(consumer, "consumer", 1000, NULL, 1, NULL);

// // //   // TODO Queue handle is not valid until you create it
// // //   switch_queue =
// // //       xQueueCreate(1, sizeof(val_e)); // Choose depth of item being our enum (1 should be okay for this
// example)
// // //                                       //**********
// // //                                       // INIT HERE****
// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   xTaskCreate(producer_midterm, "producer", 1024, NULL, 1, NULL);
// // //   xTaskCreate(consumer_midterm, "consumer", 1024, NULL, 1, NULL);
// // //   q = xQueueCreate(1, sizeof(int)); // Choose depth of item being our enum (1 should be okay for this
// example)
// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   uart_lab__init(UART_2, clock__get_core_clock_hz, 115200);
// // //   //**SELECT UART PINS & PIN MODES**
// // //   LPC_IOCON->P2_8 &= ~0b111; // CLEAR
// // //   LPC_IOCON->P2_8 |= 0b010;  // SET FUNCTIONALITY TO UART 2 TRANSMIT

// // //   LPC_IOCON->P2_9 &= ~0b111; // CLEAR
// // //   LPC_IOCON->P2_9 |= 0b010;  // SET FUNCTIONALITY TO UART 2 TRANSMIT

// // //   LPC_GPIO2->DIR |= (1 << 8);
// // //   LPC_GPIO2->DIR &= ~(1 << 9);
// // //   NVIC_EnableIRQ(GPIO_IRQn); // Enable interrupt gate for the GPIO
// // //   uart__enable_receive_interrupt(UART_2);
// // //   xTaskCreate(uart_read_task, "read", 2000, NULL, 1, NULL);
// // //   xTaskCreate(uart_write_task, "write", 2000, NULL, 1, NULL);

// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   const uint32_t spi_clock_mhz = 24;
// // //   ssp2__init(spi_clock_mhz);

// // //   spi_bus_mutex = xSemaphoreCreateMutex();

// // //   xTaskCreate(spi_id_verification_task, "spi", 2000, NULL, 1, NULL);
// // //   xTaskCreate(spi_id_verification_task, "spi2", 2000, NULL, 1, NULL);

// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   adc_to_pwm_task_queue = xQueueCreate(1, sizeof(int));
// // //   xTaskCreate(pwm_task, "pwm_task", 2000, NULL, 1, NULL);
// // //   xTaskCreate(adc_task, "adc_task", 500 / sizeof(void), NULL, 1, NULL);
// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   xTaskCreate(pwm_task, "pwm_task", 2000, NULL, 1, NULL);
// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   NVIC_EnableIRQ(GPIO_IRQn); // Enable interrupt gate for the GPIO
// // //   gpio0__attach_interrupt(30, GPIO_INTR__RISING_EDGE, pin30_isr);
// // //   gpio0__attach_interrupt(29, GPIO_INTR__FALLING_EDGE, pin29_isr);
// // // }

// // // void main(void) {
// // //   switch_pressed_signal = xSemaphoreCreateBinary(); // Create your binary semaphore

// // //   config_interrupt();        // TODO: Setup interrupt by re-using code from Part 0
// // //   NVIC_EnableIRQ(GPIO_IRQn); // Enable interrupt gate for the GPIO

// // //   xTaskCreate(sleep_on_sem_task, "sem", (512U * 4) / sizeof(void *), NULL, PRIORITY_LOW, NULL);
// // //   vTaskStartScheduler();
// // // }

// // // void main(void) {
// // //   LPC_GPIOINT->IO0IntEnR |= (1 << 29);

// // //   NVIC_EnableIRQ(GPIO_IRQn);

// // //   LPC_GPIO1->DIR |= (1 << 26);
// // //   LPC_GPIO1->DIR &= ~(1 << 29);

// // //   while (1) {
// // //     LPC_GPIO1->PIN |= (1 << 26);
// // //     delay__ms(500);
// // //     LPC_GPIO1->PIN &= ~(1 << 26);
// // //     delay__ms(500);
// // //   }

// // // }

// // // void main(void) {
// // //   sj2_cli__init();
// // //   switch_press_indication = xSemaphoreCreateBinary();

// // //   // static port_pin_s led0 = {26};
// // //   // static port_pin_s led1 = {15};
// // //   static port_pin_s switch_button = {15};

// // //   xTaskCreate(switch_task, "SWITCH", 2048, &switch_button, 1, NULL);
// // //   // xTaskCreate(led_task, "LED", 2048, &led1, 1, NULL);

// // //   // xTaskCreate(led_task, "LED", 2048, &led0, 1, NULL);

// // //   xTaskCreate(producer, "producer", 1000, NULL, 1, NULL);
// // //   xTaskCreate(consumer, "consumer", 1000, NULL, 1, NULL);

// // //   // TODO Queue handle is not valid until you create it
// // //   switch_queue =
// // //       xQueueCreate(1, sizeof(switch_e)); // Choose depth of item being our enum (1 should be okay for this
// // example)
// // //                                          //**********
// // //                                          // INIT HERE****
// // //   vTaskStartScheduler();
// // // }

// //
// //*****************************************************************************************************************************************************
// // // void uart_read_task(void *p) {
// // //   while (1) {
// // //     // TODO: Use uart_lab__polled_get() function and printf the received value
// // //     char c;
// // //     vTaskDelay(500);
// // //     uart_lab__polled_get(UART_2, &c);
// // //     fprintf(stderr, "RECIEVED: %c\n", c);
// // //   }
// // // }

// // // void uart_read_ISR_task(void *p) {
// // //   while (1) {
// // //     // TODO: Use uart_lab__polled_get() function and printf the received value
// // //     char c;
// // //     if (uart_lab__get_char_from_queue(&c, 1))
// // //       fprintf(stderr, "RECIEVED: %c\n", c);
// // //   }
// // // }

// // // void uart_write_task(void *p) {
// // //   while (1) {
// // //     // TODO: Use uart_lab__polled_put() function and send a value
// // //     uart_lab__polled_put(UART_2, 'B');
// // //     vTaskDelay(500);
// // //   }
// // // }

// // // // Private queue handle of our uart_lab.c
// // // QueueHandle_t your_uart_rx_queue;

// // // void your_receive_interrupt(void) {
// // //   // fprintf(stderr, "HI");
// // //   uint32_t intrpt_id;
// // //   if ((LPC_UART2->IIR & (2 << 1))) { // IF INTRPT
// // //     // intrpt_id = LPC_UART2->IIR & (0b111 << 1); // IDENTIFY INTRPT
// // //     while (!((LPC_UART2->LSR) & (1 << 0))) {
// // //       ;
// // //     }
// // //     const char byte = LPC_UART2->RBR;
// // //     xQueueSendFromISR(your_uart_rx_queue, &byte, NULL);
// // //   }
// // // }

// // // Public function to enable UART interrupt
// // // TODO Declare this at the header file
// // // void uart__enable_receive_interrupt(uart_number_e uart_number) {
// // //   lpc_peripheral__enable_interrupt(LPC_PERIPHERAL__UART2, your_receive_interrupt, "TRACE");
// // //   LPC_UART2->IER |= (1 << 0);
// // //   NVIC_EnableIRQ(UART2_IRQn);
// // //   your_uart_rx_queue = xQueueCreate(10, sizeof(char)); // length & item size
// // // }

// // // bool uart_lab__get_char_from_queue(char *input_byte, uint32_t timeout) {
// // //   return xQueueReceive(your_uart_rx_queue, input_byte, timeout);
// // // }

// // // // This task is done for you, but you should understand what this code is doing
// // // void board_1_sender_task(void *p) {
// // //   char number_as_string[16] = {0};

// // //   while (true) {
// // //     const int number = rand();
// // //     sprintf(number_as_string, "%i", number);

// // //     // Send one char at a time to the other board including terminating NULL char
// // //     for (int i = 0; i <= strlen(number_as_string); i++) {
// // //       uart_lab__polled_put(UART_2, number_as_string[i]);
// // //       printf("Sent: %c\n", number_as_string[i]);
// // //     }

// // //     printf("Sent: %i over UART to the other board\n", number);
// // //     vTaskDelay(3000);
// // //   }
// // // }

// // // void board_2_receiver_task(void *p) {
// // //   char number_as_string[16] = {0};
// // //   int counter = 0;

// // //   while (true) {
// // //     char byte = 0;
// // //     uart_lab__get_char_from_queue(&byte, portMAX_DELAY);
// // //     printf("Received: %c\n", byte);

// // //     // This is the last char, so print the number
// // //     if ('\0' == byte) {
// // //       number_as_string[counter] = '\0';
// // //       counter = 0;
// // //       printf("Received this number from the other board: %s\n", number_as_string);
// // //     }
// // //     // We have not yet received the NULL '\0' char, so buffer the data
// // //     else {
// // //       // for (counter = 1; counter < 16; counter++) {
// // //       //   number_as_string[counter] = byte;
// // //       // }
// // //       if (counter != 16) {
// // //         number_as_string[counter] = byte;
// // //         counter++;
// // //       }
// // //     }
// // //   }
// // // }

// // // void main(void) {
// // //   // TODO: Use uart_lab__init() function and initialize UART2 or UART3 (your choice)
// // //   uart_lab__init(UART_2, 96 * 1000 * 1000, 115200);
// // //   uart__enable_receive_interrupt(UART_2);
// // //   xTaskCreate(board_1_sender_task, "uart_write_task", 2048 / sizeof(void *), NULL, 1, NULL);
// // //   xTaskCreate(board_2_receiver_task, "uart_read_task", 2048 / sizeof(void *), NULL, 1, NULL);

// // //   vTaskStartScheduler();
// // // }

#include "ssp2_lab.h"
#include "lpc40xx.h"
#include <stdint.h>

void ssp2__init(uint32_t max_clock_mhz) {
  // Refer to LPC User manual and setup the register bits correctly
  // a) Power on Peripheral
  // b) Setup control registers CR0 and CR1
  // c) Setup prescalar register to be <= max_clock_mhz

  const uint32_t spi2_power_bit = (1 << 20);
  LPC_SC->PCONP |= spi2_power_bit;

  const uint32_t scr_value = 0;
  LPC_SSP2->CR0 = (0b111 << 0) | (scr_value << 8);

  const uint32_t ssp2_enable = (1 << 1);
  LPC_SSP2->CR1 = ssp2_enable;

  LPC_SSP2->CPSR = 4; // 96/4 = 24Mhz
}

uint8_t ssp2_exchange_byte(uint8_t data_out) {
  // Configure the Data register(DR) to send and receive data by checking the SPI peripheral status register
  LPC_SSP2->DR = data_out;
  while (LPC_SSP2->SR & (1 << 4)) { // WHILE BUSY
    ;
  }
  return LPC_SSP2->DR;
}
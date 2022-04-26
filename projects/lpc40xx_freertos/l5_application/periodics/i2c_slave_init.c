#include "i2c_slave_init.h"

void i2c2__slave_init(uint8_t slave_address_to_respond_to) {

  LPC_I2C2->CONSET |= (1 << 6) | (1 << 2);
  LPC_I2C2->ADR0 |= slave_address_to_respond_to;
  LPC_I2C2->MASK0 &= 0;
}
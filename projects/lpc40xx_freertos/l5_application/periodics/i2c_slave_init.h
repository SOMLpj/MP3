#pragma once
#include "FreeRTOS.h"
#include "lpc40xx.h"
#include <gpio.h>
#include <stdbool.h>
#include <stdio.h>
void i2c2__slave_init(uint8_t slave_address_to_respond_to);

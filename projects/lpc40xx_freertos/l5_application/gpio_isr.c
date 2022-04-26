#include "gpio_isr.h"

// @file gpio_isr.c
#include "gpio_isr.h"

// Note: You may want another separate array for falling vs. rising edge callbacks
static function_pointer_t gpio0R_callbacks[32];
static function_pointer_t gpio0F_callbacks[32];

void gpio0__attach_interrupt(uint32_t pin, gpio_interrupt_e interrupt_type, function_pointer_t callback) {

  if (interrupt_type == GPIO_INTR__RISING_EDGE) {
    gpio0R_callbacks[pin] = callback; // attaching the ISR
    LPC_GPIOINT->IO0IntEnR |= (1 << pin);
  } else {
    gpio0F_callbacks[pin] = callback; // attaching the ISR
    LPC_GPIOINT->IO0IntEnF |= (1 << pin);
  }
}

void gpio0__interrupt_dispatcher(void) {
  int intrpt_pin;
  function_pointer_t attached_user_handler;

  for (int i = 0; i < 32; i++) {
    if (LPC_GPIOINT->IO0IntStatF & (1 << i)) {
      intrpt_pin = i;
      attached_user_handler = gpio0F_callbacks[intrpt_pin];
      break;
    }

    if (LPC_GPIOINT->IO0IntStatR & (1 << i)) {
      intrpt_pin = i;
      attached_user_handler = gpio0R_callbacks[intrpt_pin];
      break;
    }
  }

  attached_user_handler();
  LPC_GPIOINT->IO0IntClr |= (1 << intrpt_pin);
}

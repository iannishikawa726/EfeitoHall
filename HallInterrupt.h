#pragma once
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_intr_alloc.h>
#include <esp_memory_utils.h>

namespace Simples {

// Instala o caminho GPIO direto em IRAM, sem o wrapper attachInterrupt do Arduino.
// O chamador tambem deve manter em IRAM/DRAM todo codigo/dado usado pelo callback.
// ESP_ERR_INVALID_STATE significa que ja existe um servico GPIO: suas flags nao
// podem ser verificadas por esta API, portanto nao o reutilizamos nem removemos.
// Deve ser chamada uma unica vez no setup, antes de liberar o motor.
inline esp_err_t iniciarHallInterrupt(gpio_num_t pino, gpio_isr_t callback, void* argumento) {
  const int numero = static_cast<int>(pino);
  if (numero < 0 || numero >= GPIO_NUM_MAX || !GPIO_IS_VALID_OUTPUT_GPIO(pino) ||
      !callback || !esp_ptr_in_iram(reinterpret_cast<const void*>(callback))) {
    // No ESP32 classico, os GPIOs de entrada apenas (34..39) nao tem pull-up.
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t erro = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  if (erro != ESP_OK) return erro; // Inclui servico preexistente, sem assumir IRAM.

  gpio_config_t entrada{};
  entrada.pin_bit_mask = 1ULL << numero;
  entrada.mode = GPIO_MODE_INPUT;
  entrada.pull_up_en = GPIO_PULLUP_ENABLE;
  entrada.pull_down_en = GPIO_PULLDOWN_DISABLE;
  entrada.intr_type = GPIO_INTR_DISABLE;
  erro = gpio_config(&entrada);
  if (erro != ESP_OK) return erro;

  erro = gpio_isr_handler_add(pino, callback, argumento);
  if (erro != ESP_OK) return erro;

  erro = gpio_set_intr_type(pino, GPIO_INTR_NEGEDGE);
  if (erro == ESP_OK) erro = gpio_intr_enable(pino);
  if (erro != ESP_OK) {
    // Limpeza limitada ao nosso pino. O erro original e preservado; o chamador
    // deve bloquear o motor. Nunca desinstala o servico global de outras bibliotecas.
    (void)gpio_intr_disable(pino);
    (void)gpio_isr_handler_remove(pino);
  }
  return erro;
}

} // namespace Simples

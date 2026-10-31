#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Config {
// Wi-Fi e identificacao: 
constexpr char WIFI_SSID[] = "3E Unicamp"; // Nome da rede Wifi
constexpr char WIFI_PASSWORD[] = "3esquilo123"; // Senha da rede Wifi
constexpr char DEVICE_ID[] = "ESP32-SP-0156"; // ID do device
constexpr char FIRMWARE_VERSION[] = "simples-1.3.8";
constexpr char API_BASE[] = "https://delta-api-mongo.onrender.com";
constexpr char SUMMARY_PATH[] = "/api/telemetry/consumption-summary";
constexpr char STATUS_PATH[] = "/api/telemetry/device-status";

// Conexões
constexpr uint8_t MOTOR_IN1 = 25; // Conexão 1 Motor 
constexpr uint8_t MOTOR_IN2 = 26; // Conexão 2 Motor
constexpr uint8_t POT_ADC = 35;      // ADC1: cursor do potenciometro, entre 3V3 e GND.
constexpr uint8_t HALL = 33;         // Sinal compativel com 3,3 V.
constexpr uint8_t SDA = 21;
constexpr uint8_t SCL = 22;
constexpr uint8_t OLED_ADDRESS = 0x3C;

// LED
constexpr bool LEDS_ENABLED = true;
constexpr uint8_t LED_GREEN = 13;  // Modo normal.
constexpr uint8_t LED_RED = 14;    // Modo anomalo.
constexpr uint8_t LED_YELLOW = 32; // Desligado, partida ou pausa de protecao.
constexpr uint32_t PWM_HZ = 20000;
constexpr uint8_t PWM_BITS = 10;
constexpr uint8_t SPEEDS[] = {0, 70, 100};
constexpr const char* MODE_NAMES[] = {"Desligado", "Modo normal", "Modo anomalo"};
constexpr uint16_t POT_NORMAL_MIN = 1365;
constexpr uint16_t POT_ANOMALO_MIN = 2730;
constexpr uint16_t POT_HYSTERESIS = 80; // Evita alternancia na fronteira entre faixas.
constexpr uint32_t POT_SAMPLE_MS = 20;
constexpr uint32_t POT_STABLE_MS = 150;

// Frequencia Motor
constexpr double MOTOR_NORMAL_HZ = 5.0 / 17.0;
constexpr double MOTOR_ANOMALO_HZ = 5.0 / 7.0;

// Pulsos CONTADOS por volta, depois do divisor. Roda atual: 2 imas.
constexpr uint8_t PULSES_PER_REVOLUTION = 2;
// Pulsos/L SIMULADOS:
constexpr double PULSES_PER_LITER = 10.0;
// TODA borda, aceita ou rejeitada, renova o intervalo de 3000 us.
constexpr uint32_t HALL_MIN_PULSE_US = 3000;
// so é correto se houver 2 bordas validas por passagem de ima. Senao use 1.
constexpr uint32_t HALL_PULSE_DIVIDER = 2;
// Vazao local: pulsos reais acumulados em ate tres intervalos de amostragem.


constexpr uint32_t SAMPLE_MS = 1000;
constexpr uint32_t WINDOW_MS = 5UL * 60UL * 1000UL; // Fixo: detector atual conta 6 janelas = 30 min.
constexpr uint32_t STATUS_MS = 60UL * 1000UL;
constexpr uint32_t DISPLAY_MS = 250;
constexpr uint32_t WIFI_RETRY_MS = 15000;
constexpr uint32_t RETRY_MS = 30000; // Nova tentativa, sem espera exponencial.
// A API ja levou mais de 35 s para responder. A espera de leitura usa ms e uint16_t.
constexpr uint16_t HTTP_TIMEOUT_MS = 60000;
constexpr uint32_t HTTP_BODY_TIMEOUT_MS = 10000; // Prazo total separado para ler o corpo.
constexpr size_t HTTP_BODY_MAX_BYTES = 32768;
constexpr int32_t HTTP_CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t TLS_HANDSHAKE_TIMEOUT_S = 15;


// Baterias
constexpr bool BATTERY_ENABLED = true;
constexpr uint8_t BATTERY_ADC = 34;  // Conexão medir bateria


constexpr double BATTERY_R_TOP_OHMS = 47000.0;
constexpr double BATTERY_R_BOTTOM_OHMS = 10000.0;
constexpr double BATTERY_CALIBRATION = 1.0; // Ajuste comparando com multimetro.
// Contorno: atualizar apenas parado, com o potenciometro realmente no minimo.
// Durante o giro, o display guarda a ultima referencia, que pode ficar antiga.
constexpr uint32_t BATTERY_REST_MS = 15000;
constexpr uint16_t BATTERY_REST_POT_MAX = 100;
// Referencias ajustaveis de exibicao, nao uma medicao exata da carga restante.
constexpr double BATTERY_EMPTY_V = 4.0; // 1,0 V por pilha -> 0%.
constexpr double BATTERY_FULL_V = 6.0;  // 1,6 V por pilha -> 100%.
}



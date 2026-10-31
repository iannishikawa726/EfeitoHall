// ESP32 classico. Abra este arquivo e configure Config.h.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_arduino_version.h>
#include <esp_timer.h>
#include <esp_sntp.h>
#include <time.h>
#include "Config.h"
#include "Logica.h"
#include "Certificado.h"
#include "ArduinoJson.h"
#include "HallInterrupt.h"
#include "HttpBody.h"

#if !defined(CONFIG_IDF_TARGET_ESP32)
#error "Pinos definidos para ESP32 classico"
#endif
static_assert(Config::PULSES_PER_LITER > 0 && Config::PULSES_PER_REVOLUTION > 0 &&
              Config::HALL_PULSE_DIVIDER > 0, "Calibracao invalida");
static_assert(sizeof(Config::DEVICE_ID) <= 101, "device_id muito longo");
static_assert(Config::WINDOW_MS == Simples::JANELA_MS && Config::BATTERY_R_BOTTOM_OHMS > 0,
              "A analise atual do backend requer janelas de cinco minutos");
static_assert(Config::BATTERY_R_TOP_OHMS > 0 && Config::BATTERY_CALIBRATION > 0 &&
              Config::BATTERY_EMPTY_V >= 0 && Config::BATTERY_FULL_V > Config::BATTERY_EMPTY_V,
              "Calibracao da bateria invalida");
static_assert(Config::BATTERY_REST_MS >= Config::SAMPLE_MS &&
              Config::BATTERY_REST_POT_MAX < Config::POT_NORMAL_MIN - Config::POT_HYSTERESIS,
              "Repouso da bateria requer potenciometro no minimo e tempo de estabilizacao");
static_assert(Config::POT_NORMAL_MIN > Config::POT_HYSTERESIS &&
              Config::POT_ANOMALO_MIN + Config::POT_HYSTERESIS <= 4095 &&
              Config::POT_NORMAL_MIN + 2 * Config::POT_HYSTERESIS < Config::POT_ANOMALO_MIN,
              "Faixas do potenciometro invalidas");
static_assert(Config::POT_ADC == 35 && Config::POT_ADC != Config::BATTERY_ADC,
              "Potenciometro usa GPIO35, ADC1 separado da bateria");
constexpr bool pinoLedDisponivel(uint8_t pino) {
  return GPIO_IS_VALID_OUTPUT_GPIO(pino) && pino != Config::MOTOR_IN1 &&
    pino != Config::MOTOR_IN2 && pino != Config::POT_ADC && pino != Config::HALL &&
    pino != Config::SDA && pino != Config::SCL && pino != Config::BATTERY_ADC;
}
static_assert(!Config::LEDS_ENABLED ||
  (pinoLedDisponivel(Config::LED_GREEN) && pinoLedDisponivel(Config::LED_RED) &&
   pinoLedDisponivel(Config::LED_YELLOW) && Config::LED_GREEN != Config::LED_RED &&
   Config::LED_GREEN != Config::LED_YELLOW && Config::LED_RED != Config::LED_YELLOW),
  "LEDs precisam de GPIOs de saida distintos e livres");

Adafruit_SSD1306 tela(128, 64, &Wire, -1);
Preferences armazenamento;
Simples::Memoria memoria{};
Simples::HallFilter filtro(Config::HALL_MIN_PULSE_US, Config::HALL_PULSE_DIVIDER);
Simples::MediaVazao mediaVazao;
Simples::ControlePotenciometro controlePot(Config::POT_NORMAL_MIN, Config::POT_ANOMALO_MIN,
                                         Config::POT_HYSTERESIS, Config::POT_STABLE_MS);
Simples::JanelasFixas janelas;
portMUX_TYPE hallMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE horaMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE bateriaApiMux = portMUX_INITIALIZER_UNLOCKED;
int nivelBateriaParaApi = -1; // Compartilhado com a rede; -1 = sem leitura valida.
volatile uint64_t pulsos = 0;
int64_t ajusteUtc = 0;
QueueHandle_t paraEnviar, resultadoEnvio, mensagensRede;
bool telaOk = false, motorOk = false, hallOk = false, memoriaAberta = false, memoriaBloqueada = false;
bool memoriaAlterada = false, enviando = false, zeroSemHora = false;
bool pausaMemoria = false, falhaGravacao = false, avisouExcedente = false;
bool zeroNestaLigacao = false;
double vazaoHall = 0;
Simples::IndicadorBateria bateria(Config::BATTERY_EMPTY_V, Config::BATTERY_FULL_V);
Simples::RepousoBateria repousoBateria(Config::BATTERY_REST_MS);
double bateriaAdcMv = NAN, bateriaVolts = NAN;
int64_t ultimaMedicaoBateria = -1;
uint64_t zeroPulsos = 0;
uint64_t zeroPulsosDisplay = 0;
int64_t inicioTesteLocalMs = 0;
int64_t ultimaAmostra = 0, ultimaTela = -1000, proximoEnvio = 0, proximaGravacao = 0;
uint8_t modoMotor = 0;
uint16_t valorPot = 0;
int64_t ultimaLeituraPot = -1000;
bool tarefaRedeOk = false;
Simples::AvisoRede estadoRede{Simples::EstadoApi::Aguardando, 0, "aguardando"};
char linhaSerial[32]{}, mensagemApi[24] = "aguardando";
size_t usados = 0;
bool linhaLonga = false;

int64_t agoraMs() { return esp_timer_get_time() / 1000; }
uint64_t lerPulsos() {
  portENTER_CRITICAL(&hallMux);
  const uint64_t n = pulsos;
  portEXIT_CRITICAL(&hallMux);
  return n;
}
uint64_t pulsosNoDisplay(uint64_t n) {
  return n >= zeroPulsosDisplay ? n - zeroPulsosDisplay : 0;
}
int lerNivelBateriaParaApi() {
  portENTER_CRITICAL(&bateriaApiMux);
  const int nivel = nivelBateriaParaApi;
  portEXIT_CRITICAL(&bateriaApiMux);
  return nivel;
}
void IRAM_ATTR pulsoHall(void*) {
  portENTER_CRITICAL_ISR(&hallMux);
  const int64_t us = esp_timer_get_time();
  if (filtro.edge(us)) { pulsos = pulsos + 1; janelas.contar(us); }
  portEXIT_CRITICAL_ISR(&hallMux);
}
double totalAtual(uint64_t n) { return Simples::litrosDosPulsos(n, Config::PULSES_PER_LITER); }
int64_t utcDe(int64_t ms) {
  portENTER_CRITICAL(&horaMux);
  const int64_t ajuste = ajusteUtc;
  portEXIT_CRITICAL(&horaMux);
  return ajuste ? ms + ajuste : 0;
}
void sincronizou(struct timeval* tv) {
  if (tv->tv_sec < 1704067200LL) return;
  const int64_t ajuste = static_cast<int64_t>(tv->tv_sec) * 1000 + tv->tv_usec / 1000 - agoraMs();
  portENTER_CRITICAL(&horaMux);
  if (!ajusteUtc) ajusteUtc = ajuste; // Mantem as janelas estaveis nesta inicializacao.
  portEXIT_CRITICAL(&horaMux);
}
String dataIso(int64_t ms) {
  const time_t segundos = ms / 1000;
  struct tm t;
  gmtime_r(&segundos, &t);
  char data[24], texto[32];
  strftime(data, sizeof(data), "%Y-%m-%dT%H:%M:%S", &t);
  snprintf(texto, sizeof(texto), "%s.%03uZ", data, static_cast<unsigned>(ms % 1000));
  return texto;
}

// 1. Memoria: somente o loop acessa a fila e a flash. A tarefa de rede recebe copias.
void carregarMemoria() {
  // Executada ANTES de criar a tarefa de rede: nenhum pendente antigo e despachado.
  if (!Simples::novaSessao(memoria, Config::DEVICE_ID)) {
    memoriaBloqueada = true;
    Serial.println("[MEM] Identificador invalido; motor e consumo bloqueados.");
    return;
  }
  memoriaAberta = armazenamento.begin("hall_simples", false);
  // Limpa SOMENTE o namespace deste projeto, incluindo a antiga chave legado.
  if (!memoriaAberta || !armazenamento.clear()) {
    memoriaBloqueada = true;
    Serial.println("[MEM] Falha ao limpar memoria do teste; motor e consumo bloqueados. Reinicie para tentar novamente.");
    return;
  }
  memoriaAlterada = true;
  salvarMemoria();
  Serial.println("[TESTE] Nova sessao: fila anterior apagada; total inicial 0.000 L. Somente novas janelas serao enviadas.");
}
bool salvarMemoria() {
  if (memoriaBloqueada) return false;
  if (!memoriaAlterada) return true;
  if (agoraMs() < proximaGravacao) return false;
  if (armazenamento.putBytes("dados", &memoria, sizeof(memoria)) == sizeof(memoria)) {
    memoriaAlterada = false;
    falhaGravacao = false;
    return true;
  }
  Serial.println("[MEM] Falhou ao salvar; dados mantidos na RAM. Nova tentativa em 10 s.");
  falhaGravacao = true;
  proximaGravacao = agoraMs() + 10000;
  return false;
}
bool guardarJanela(int64_t deMs, int64_t ateMs, double litros) {
  if (memoriaBloqueada || !utcDe(0)) return false;
  const Simples::Resumo r{utcDe(deMs), utcDe(ateMs), litros};
  if (!Simples::janelaCompativel(r) || !Simples::adicionar(memoria, r)) return false;
  memoria.totalLitros += litros;
  memoriaAlterada = true;
  Serial.printf("[JANELA] %.3f L; fila=%u.\n", litros, static_cast<unsigned>(memoria.quantidade));
  return true;
}
void cuidarEnvios() {
  bool confirmado;
  if (resultadoEnvio && xQueueReceive(resultadoEnvio, &confirmado, 0) == pdTRUE) {
    enviando = false;
    if (confirmado && memoria.quantidade) {
      // Reinicia apenas a apresentacao. ISR, vazao e janelas seguem acumulando.
      zeroPulsosDisplay = lerPulsos();
      Simples::removerPrimeiro(memoria);
      memoriaAlterada = true;
      ultimaTela = -1000;
      Serial.println("[DISPLAY] Resumo confirmado: pulsos e total do display zerados.");
    }
    proximoEnvio = agoraMs() + (confirmado ? 250 : Config::RETRY_MS);
  }
  if (zeroSemHora && utcDe(0)) {
    memoria.inicioTesteMs = utcDe(inicioTesteLocalMs);
    memoriaAlterada = true;
    zeroSemHora = false;
    Serial.println("[TESTE] start=" + dataIso(memoria.inicioTesteMs));
  }
  const bool salvo = salvarMemoria();
  if (salvo && tarefaRedeOk && paraEnviar && !enviando && memoria.quantidade && utcDe(0) && agoraMs() >= proximoEnvio)
    enviando = xQueueSend(paraEnviar, &memoria.fila[0], 0) == pdTRUE;
}

void fecharJanela() {
  int64_t inicio;
  uint64_t n;
  portENTER_CRITICAL(&hallMux);
  janelas.avancar(esp_timer_get_time());
  const bool fechou = janelas.consultar(inicio, n);
  portEXIT_CRITICAL(&hallMux);
  if (!fechou) return;
  // Durante a pausa, omite somente janelas vazias que nao cabem mais na fila.
  // Janelas com pulsos ficam na RAM, com suas datas originais, ate poderem ser salvas.
  const bool omitirVazia = memoria.quantidade >= Simples::LIMITE_FILA && !n;
  if (omitirVazia || guardarJanela(inicio, inicio + Config::WINDOW_MS, n / Config::PULSES_PER_LITER)) {
    portENTER_CRITICAL(&hallMux);
    janelas.confirmar();
    portEXIT_CRITICAL(&hallMux);
  }
}

// 2. Circuito, potenciometro e comandos: nao esperam pela internet.
double frequenciaMotor() {
  return Simples::frequenciaEstimada(modoMotor, Config::MOTOR_NORMAL_HZ, Config::MOTOR_ANOMALO_HZ);
}
void atualizarLeds() {
  if (!Config::LEDS_ENABLED) return;
  // Apaga os tres antes de acender o indicador do comando efetivo.
  digitalWrite(Config::LED_GREEN, LOW);
  digitalWrite(Config::LED_RED, LOW);
  digitalWrite(Config::LED_YELLOW, LOW);
  const uint8_t modo = motorOk && !pausaMemoria ? modoMotor : 0;
  const uint8_t pino = modo == 1 ? Config::LED_GREEN :
    modo == 2 ? Config::LED_RED : Config::LED_YELLOW;
  digitalWrite(pino, HIGH);
}
void aplicarMotor() {
  atualizarLeds();
  if (!motorOk) return;
  digitalWrite(Config::MOTOR_IN2, LOW);
  const uint32_t duty = pausaMemoria ? 0 : ((1UL << Config::PWM_BITS) - 1) * Config::SPEEDS[modoMotor] / 100;
  if (duty) repousoBateria.observar(false, agoraMs());
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(Config::MOTOR_IN1, duty);
#else
  ledcWrite(0, duty);
#endif
}
void protegerRegistros() {
  portENTER_CRITICAL(&hallMux);
  janelas.avancar(esp_timer_get_time());
  const int64_t atrasadas = janelas.atrasadas();
  const uint64_t excedentes = janelas.excedentes();
  portEXIT_CRITICAL(&hallMux);
  const bool pausar = !hallOk || Simples::devePausar(memoria.quantidade, atrasadas, excedentes, memoriaBloqueada, falhaGravacao);
  if (pausar != pausaMemoria) {
    pausaMemoria = pausar;
    if (pausar) {
      modoMotor = 0; controlePot.bloquear(); aplicarMotor();
      if (!hallOk) Serial.println("[PAUSA] Motor OFF: interrupcao Hall indisponivel.");
      else Serial.println("[PAUSA] Motor OFF para preservar registros. Reservada a ultima vaga para a desaceleracao.");
    } else Serial.println("[PAUSA] Espaco e gravacao disponiveis. Passe o potenciometro por DESLIGADO antes de ligar.");
  }
  if (excedentes && !avisouExcedente) {
    avisouExcedente = true;
    Serial.println("[MEM] Movimento com motor bloqueado excedeu o buffer RAM. Total continua contando; ha pulsos sem janela. Ver STATUS.");
  }
}
void imprimirStatus() {
  const uint64_t n = lerPulsos();
  const uint64_t exibidos = pulsosNoDisplay(n);
  Serial.printf("[STATUS] %s; freq_est=%.1fHz (voltas/s); vazao_Hall=%.3fL/min pulsos_display=%llu total_display=%.3fL fila=%u\n",
    Config::MODE_NAMES[modoMotor], frequenciaMotor(), vazaoHall,
    static_cast<unsigned long long>(exibidos), totalAtual(exibidos), static_cast<unsigned>(memoria.quantidade));
  Serial.printf("[POT] ADC=%u; habilitado=%s. Frequencia e referencia do modo; litros e vazao usam Hall.\n",
    valorPot, controlePot.habilitado() ? "sim" : "nao: passe por desligado");
  Serial.printf("[DIAG] Hall=%s rede=%s RSSI=%d dBm heap_livre=%u B timeout_leitura=%u ms\n",
    hallOk ? "OK" : "ERRO", tarefaRedeOk ? "OK" : "ERRO", WiFi.RSSI(),
    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(Config::HTTP_TIMEOUT_MS));
  if (!Config::BATTERY_ENABLED) Serial.println("[BATERIA] Medicao desativada.");
  else if (ultimaMedicaoBateria < 0)
    Serial.println("[BATERIA] Sem referencia: deixe o potenciometro no minimo e o motor desligado por 15 s.");
  else {
    Serial.printf("[BATERIA] GPIO%u=%.1f mV; reconstruida=%.3f V; filtrada=%.3f V; referencia=%d%%; idade=%llu s\n",
      static_cast<unsigned>(Config::BATTERY_ADC), bateriaAdcMv, bateriaVolts, bateria.tensaoFiltrada(),
      bateria.porcentagem(), static_cast<unsigned long long>((agoraMs() - ultimaMedicaoBateria) / 1000));
    Serial.printf("[BATERIA] %s; calibracao=%.6f. Referencia aproximada, sem protecao de subtensao.\n",
      repousoBateria.pronto(agoraMs()) ? "atualizando em repouso" : "ultima leitura mantida",
      Config::BATTERY_CALIBRATION);
  }
  Serial.printf("[TESTE] device_id=%s; %s=%llu\n", Config::DEVICE_ID,
    zeroNestaLigacao ? "pulsos_desde_zero" : "pulsos_boot",
    static_cast<unsigned long long>(zeroNestaLigacao ? n - zeroPulsos : n));
  if (memoria.inicioTesteMs) Serial.println("[TESTE] start=" + dataIso(memoria.inicioTesteMs));
  else Serial.println("[TESTE] Aguardando NTP para informar a data de inicio desta sessao.");
  if (utcDe(0)) {
    const int64_t agora = utcDe(agoraMs());
    if (!memoria.inicioTesteMs || agora > memoria.inicioTesteMs) Serial.println("[TESTE] end=" + dataIso(agora));
    else Serial.println("[TESTE] Aguarde: primeiro resumo completo do teste fecha em " + dataIso(memoria.inicioTesteMs + Config::WINDOW_MS));
  }
  portENTER_CRITICAL(&hallMux);
  const uint64_t excedentes = janelas.excedentes();
  portEXIT_CRITICAL(&hallMux);
  Serial.printf("[MEM] pausa=%s; pulsos_sem_janela=%llu\n", pausaMemoria ? "sim" : "nao", static_cast<unsigned long long>(excedentes));
  Serial.println("[TESTE] Swagger GET consumption-summary: page=0, size=20, sort vazio.");
}
void zerar() {
  modoMotor = 0; controlePot.bloquear();
  aplicarMotor();
  portENTER_CRITICAL(&hallMux);
  janelas.avancar(esp_timer_get_time());
  const uint64_t n = pulsos;
  inicioTesteLocalMs = janelas.proximoInicioCompletoMs();
  portEXIT_CRITICAL(&hallMux);
  zeroPulsos = n;
  zeroPulsosDisplay = n;
  zeroNestaLigacao = true;
  memoria.zeroLitros = totalAtual(n);
  memoria.inicioTesteMs = utcDe(inicioTesteLocalMs);
  zeroSemHora = !memoria.inicioTesteMs;
  memoriaAlterada = true;
  Serial.println("[ZERO] Total local zerado; historico e janela atual preservados. Filtro do teste comeca na proxima janela inteira.");
  Serial.println("[ZERO] Isso nao reinicia o detector/baseline da API.");
  Serial.println("[ZERO] Passe o potenciometro por DESLIGADO antes de ligar novamente.");
  cuidarEnvios();
  imprimirStatus();
}
void lerPotenciometro() {
  const int64_t ms = agoraMs();
  if (ms - ultimaLeituraPot < Config::POT_SAMPLE_MS) return;
  ultimaLeituraPot = ms;
  uint32_t soma = 0;
  for (unsigned i = 0; i < 8; ++i) soma += analogRead(Config::POT_ADC);
  valorPot = soma / 8;
  const uint8_t novo = controlePot.atualizar(valorPot, ms, pausaMemoria || !motorOk);
  if (novo != modoMotor) {
    modoMotor = novo; aplicarMotor();
    Serial.printf("[MOTOR] %s; freq_est=%.1f Hz.\n", Config::MODE_NAMES[modoMotor], frequenciaMotor());
  }
  repousoBateria.observar(valorPot <= Config::BATTERY_REST_POT_MAX &&
    (!motorOk || pausaMemoria || modoMotor == 0), ms);
}
void lerSerial() {
  for (unsigned limite = 0; limite < 64 && Serial.available(); ++limite) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      linhaSerial[usados] = 0;
      String comando(linhaSerial); comando.trim(); comando.toUpperCase();
      if (!linhaLonga && comando == "ZERAR") zerar();
      else if (!linhaLonga && (comando == "STATUS" || comando == "TESTE")) imprimirStatus();
      else if (linhaLonga || comando.length()) Serial.println("Comandos: ZERAR | STATUS | TESTE (Nova linha). Reinicie a placa para limpar a fila.");
      usados = 0; linhaLonga = false;
    } else if (usados + 1 < sizeof(linhaSerial) && !linhaLonga) linhaSerial[usados++] = c;
    else linhaLonga = true;
  }
}
void atualizarMedidas() {
  const int64_t ms = agoraMs();
  if (ms - ultimaAmostra < Config::SAMPLE_MS) return;
  const uint64_t n = lerPulsos();
  mediaVazao.adicionar(n, ms);
  vazaoHall = mediaVazao.litrosPorMinuto(Config::PULSES_PER_LITER);
  ultimaAmostra = ms;
  if (Config::BATTERY_ENABLED && repousoBateria.pronto(ms)) {
    // Descartar a primeira conversao depois das leituras do potenciometro no ADC1.
    analogReadMilliVolts(Config::BATTERY_ADC);
    delayMicroseconds(200);
    uint32_t mv = 0;
    for (unsigned i = 0; i < 16; ++i) mv += analogReadMilliVolts(Config::BATTERY_ADC);
    bateriaAdcMv = mv / 16.0;
    bateriaVolts = bateriaAdcMv / 1000.0 * (Config::BATTERY_R_TOP_OHMS + Config::BATTERY_R_BOTTOM_OHMS) /
                     Config::BATTERY_R_BOTTOM_OHMS * Config::BATTERY_CALIBRATION;
    bateria.atualizar(bateriaVolts, ms);
    const int nivel = bateria.porcentagem();
    portENTER_CRITICAL(&bateriaApiMux);
    nivelBateriaParaApi = nivel;
    portEXIT_CRITICAL(&bateriaApiMux);
    ultimaMedicaoBateria = ms;
  }
}
void atualizarTela() {
  const int64_t ms = agoraMs();
  if (!telaOk || ms - ultimaTela < Config::DISPLAY_MS) return;
  ultimaTela = ms;
  tela.clearDisplay(); tela.setTextColor(SSD1306_WHITE); tela.setTextSize(1); tela.setCursor(0, 0);
  tela.print("DELTA ");
  if (!hallOk) tela.println("ERRO HALL OFF");
  else if (pausaMemoria) tela.println("PAUSA guardar");
  else if (motorOk && !controlePot.habilitado()) tela.println("Gire ao minimo");
  else if (motorOk) {
    const unsigned nivelPot = (static_cast<uint32_t>(valorPot) * 100U + 2047U) / 4095U;
    tela.print(modoMotor == 0 ? "Desligado" : modoMotor == 1 ? "Normal" : "Anomalo");
    // P = potenciometro. Cinco caracteres em x=98..127, sem sobrepor o modo.
    tela.setCursor(98, 0);
    tela.printf("P%3u%%\n", nivelPot);
  }
  else tela.println("ERRO PWM OFF");
  const double estimada = frequenciaMotor();
  if (estimada >= 0) tela.printf("Freq.est:%.1fHz\n", estimada);
  else tela.println("Freq.est:calibrar");
  tela.printf("Vazao Hall:%.2fL/min\n", vazaoHall);
  const uint64_t exibidos = pulsosNoDisplay(lerPulsos());
  tela.printf("Total%s:%.3fL\n", memoriaAlterada ? "*" : "", totalAtual(exibidos));
  tela.printf("Pulsos:%llu\n", static_cast<unsigned long long>(exibidos));
  if (Config::BATTERY_ENABLED && bateria.porcentagem() >= 0)
    tela.printf(repousoBateria.pronto(agoraMs()) ? "Bat.est:%d%%\n" : "Bat.ref:%d%%\n", bateria.porcentagem());
  else if (Config::BATTERY_ENABLED)
    tela.println(valorPot > Config::BATTERY_REST_POT_MAX ? "Bat: gire ao minimo" : "Bat: aguarde 15s");
  else tela.println("Bateria: desativada");
  tela.printf("Fila:%u WiFi:%s%s\n", static_cast<unsigned>(memoria.quantidade),
    WiFi.status() == WL_CONNECTED ? "OK" : "--", memoriaBloqueada ? " M!" : "");
  tela.print(mensagemApi); tela.display();
}

// 3. API: uma funcao HTTP, uma para status e uma para consumo.
void avisarRede(const char* texto, Simples::EstadoApi estado, int codigo) {
  Simples::AvisoRede aviso{estado, codigo, {}};
  strlcpy(aviso.texto, texto, sizeof(aviso.texto));
  if (mensagensRede) xQueueOverwrite(mensagensRede, &aviso);
}
void avisarResultado(const char* tipo, bool confirmado, int codigo) {
  char aviso[24];
  if (confirmado) snprintf(aviso, sizeof(aviso), "%s confirmado", tipo);
  else if (codigo == -11) snprintf(aviso, sizeof(aviso), "%s timeout", tipo);
  else snprintf(aviso, sizeof(aviso), "%s falha %d", tipo, codigo);
  avisarRede(aviso, confirmado ? Simples::EstadoApi::Confirmada : Simples::EstadoApi::Falha, codigo);
}
int chamarApi(const char* metodo, const String& rota, const String& json, String& resposta) {
  const int64_t iniciouMs = agoraMs();
  Serial.printf("[HTTP] Iniciando %s %s; RSSI=%d dBm; payload=%u B; espera=%u ms\n",
    metodo, rota.c_str(), WiFi.RSSI(), static_cast<unsigned>(json.length()),
    static_cast<unsigned>(Config::HTTP_TIMEOUT_MS));
  avisarRede(rota.startsWith(Config::STATUS_PATH) ? "enviando status" : "enviando consumo", Simples::EstadoApi::Enviando, 0);
  WiFiClientSecure cliente;
  cliente.setCACert(API_ROOT_CA);
  cliente.setHandshakeTimeout(Config::TLS_HANDSHAKE_TIMEOUT_S);
  HTTPClient http;
  http.setConnectTimeout(Config::HTTP_CONNECT_TIMEOUT_MS); http.setTimeout(Config::HTTP_TIMEOUT_MS);
  http.setReuse(false); // Cada chamada ja cria um cliente novo; pede Connection: close.
  http.setAcceptEncoding("identity"); // O corpo JSON e lido sem descompressao.
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(cliente, String(Config::API_BASE) + rota)) {
    avisarRede("URL/API invalida", Simples::EstadoApi::Falha, 0); Serial.println("[HTTP] Falhou ao iniciar a requisicao."); return 0;
  }
  const char* cabecalhos[] = {"Transfer-Encoding", "rndr-id", "X-Request-ID", "Location"};
  http.collectHeaders(cabecalhos, sizeof(cabecalhos) / sizeof(cabecalhos[0]));
  http.addHeader("Accept", "application/json");
  int codigo;
  if (!strcmp(metodo, "POST")) { http.addHeader("Content-Type", "application/json"); codigo = http.POST(json); }
  else codigo = http.GET();
  Serial.printf("[HTTP] %s %s -> %d; ate_cabecalhos=%.3f s\n",
    metodo, rota.c_str(), codigo, (agoraMs() - iniciouMs) / 1000.0);
  int retorno = codigo;
  if (codigo > 0) {
    const auto leitura = HttpBody::lerCorpoHttp(http, resposta, Config::HTTP_BODY_MAX_BYTES,
                                               Config::HTTP_BODY_TIMEOUT_MS);
    const bool completo = leitura.estado == HttpBody::Estado::Completo;
    Serial.printf("[HTTP] Corpo: %s; bytes=%u; total=%.3f s\n",
      completo ? "completo" : leitura.estado == HttpBody::Estado::Timeout ? "timeout" :
      leitura.estado == HttpBody::Estado::Limite ? "limite excedido" : "incompleto/invalido",
      static_cast<unsigned>(leitura.bytes), (agoraMs() - iniciouMs) / 1000.0);
    if (!completo && !strcmp(metodo, "GET")) {
      // Nunca confirma duplicata com documento parcial. O codigo HTTP real ja foi registrado.
      retorno = leitura.estado == HttpBody::Estado::Timeout ? HTTPC_ERROR_READ_TIMEOUT : HTTPC_ERROR_ENCODING;
    }
    const String renderId = http.header("rndr-id");
    const String requestId = http.header("X-Request-ID");
    if (renderId.length()) Serial.println("[HTTP] rndr-id=" + renderId);
    if (requestId.length()) Serial.println("[HTTP] X-Request-ID=" + requestId);
    if (codigo >= 300 && codigo < 400) Serial.println("[HTTP] Redirecionamento nao seguido: " + http.header("Location"));
  } else {
    resposta = HTTPClient::errorToString(codigo);
    if (codigo == HTTPC_ERROR_READ_TIMEOUT)
      Serial.println("[HTTP] Sem cabecalhos completos no prazo. Envio sem confirmacao; pode haver demora do servidor ou falha de rede.");
    char erroTls[160]{};
    const int tls = cliente.lastError(erroTls, sizeof(erroTls));
    if (tls) Serial.printf("[TLS] Ultimo erro de conexao registrado: %d %s\n", tls, erroTls);
  }
  Serial.println(resposta);
  http.end(); return retorno;
}
bool enviarStatus() {
  JsonDocument doc;
  const int rssi = WiFi.RSSI();
  doc["device_id"] = Config::DEVICE_ID; doc["last_ping_at"] = dataIso(utcDe(agoraMs()));
  doc["wifi_signal_rssi"] = Simples::sinalWifi(rssi);
  doc["firmware_version"] = Config::FIRMWARE_VERSION; doc["connectivity_status"] = "online";
  doc["unavailability_reason"] = nullptr;
  if (Config::BATTERY_ENABLED) {
    const int nivel = lerNivelBateriaParaApi();
    // Zero e valido; antes da primeira leitura, omite sem inventar porcentagem.
    if (nivel >= 0 && nivel <= 100) doc["battery_level"] = nivel;
  }
  String json, resposta; serializeJson(doc, json);
  Serial.println("[STATUS API] " + json);
  const int codigo = chamarApi("POST", Config::STATUS_PATH, json, resposta);
  avisarResultado("status", codigo == 200, codigo);
  return codigo == 200;
}
String codificar(const String& valor) {
  String saida;
  for (size_t i = 0; i < valor.length(); ++i) {
    const uint8_t c = valor[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') saida += char(c);
    else { char trecho[4]; snprintf(trecho, sizeof(trecho), "%%%02X", c); saida += trecho; }
  }
  return saida;
}
String normalizarData(const char* texto) {
  String s(texto ? texto : "");
  if (s.endsWith("+00:00")) { s.remove(s.length() - 6); s += 'Z'; }
  if (s.indexOf('.') >= 0 && s.endsWith("Z")) {
    s.remove(s.length() - 1);
    while (s.endsWith("0")) s.remove(s.length() - 1);
    if (s.endsWith(".")) s.remove(s.length() - 1);
    s += 'Z';
  }
  return s;
}
bool confirmarResumo(const Simples::Resumo& r) {
  String resposta;
  const String rota = String(Config::SUMMARY_PATH) + "?device_id=" + codificar(Config::DEVICE_ID) +
    "&start=" + codificar(dataIso(r.inicioMs)) + "&end=" + codificar(dataIso(r.inicioMs + 1)) + "&page=0&size=100";
  if (chamarApi("GET", rota, "", resposta) != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, resposta)) return false;
  for (JsonObjectConst item : doc["content"].as<JsonArrayConst>()) {
    if (strcmp(item["device_id"] | "", Config::DEVICE_ID) ||
        normalizarData(item["window_started_at"] | "") != normalizarData(dataIso(r.inicioMs).c_str()) ||
        normalizarData(item["window_finished_at"] | "") != normalizarData(dataIso(r.fimMs).c_str()) ||
        !item["consumption_liters"].is<double>() || !item["lpm_average"].is<double>()) continue;
    if (fabs(item["consumption_liters"].as<double>() - r.litros) <= 1e-6 * fmax(1, fabs(r.litros)) &&
        fabs(item["lpm_average"].as<double>() - Simples::mediaLpm(r)) <= 1e-6 * fmax(1, fabs(Simples::mediaLpm(r)))) return true;
  }
  return false;
}
bool enviarConsumo(const Simples::Resumo& r) {
  if (!Simples::janelaCompativel(r)) return false;
  JsonDocument doc;
  doc["device_id"] = Config::DEVICE_ID; doc["window_started_at"] = dataIso(r.inicioMs);
  doc["window_finished_at"] = dataIso(r.fimMs); doc["consumption_liters"] = r.litros;
  doc["lpm_average"] = Simples::mediaLpm(r);
  String json, resposta; serializeJson(doc, json); Serial.println("[RESUMO] " + json);
  const int codigo = chamarApi("POST", Config::SUMMARY_PATH, json, resposta);
  const bool ok = codigo == 201 || (codigo == 409 && confirmarResumo(r));
  avisarResultado("consumo", ok, codigo);
  return ok;
}
void tarefaRede(void*) {
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
  sntp_set_time_sync_notification_cb(sincronizou);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  int64_t tentouWifi = agoraMs(), tentouNtp = agoraMs(), proximoStatus = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      avisarRede("sem WiFi", Simples::EstadoApi::Falha, 0);
      if (agoraMs() - tentouWifi >= Config::WIFI_RETRY_MS) { tentouWifi = agoraMs(); WiFi.reconnect(); }
    } else if (!utcDe(0)) {
      avisarRede("aguardando NTP", Simples::EstadoApi::Aguardando, 0);
      if (agoraMs() - tentouNtp >= 60000) { esp_sntp_restart(); tentouNtp = agoraMs(); }
    } else {
      Simples::Resumo r;
      if (xQueueReceive(paraEnviar, &r, 0) == pdTRUE) {
        const bool ok = enviarConsumo(r);
        xQueueSend(resultadoEnvio, &ok, portMAX_DELAY);
      }
      if (agoraMs() >= proximoStatus) {
        const bool ok = enviarStatus();
        proximoStatus = agoraMs() + (ok ? Config::STATUS_MS : Config::RETRY_MS);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// 4. Inicializacao e repeticao principal.
void setup() {
  Serial.begin(115200);
  Serial.printf("\nHall %s; %s\n", Config::FIRMWARE_VERSION, Config::DEVICE_ID);
  if (Config::LEDS_ENABLED) {
    pinMode(Config::LED_GREEN, OUTPUT); digitalWrite(Config::LED_GREEN, LOW);
    pinMode(Config::LED_RED, OUTPUT); digitalWrite(Config::LED_RED, LOW);
    pinMode(Config::LED_YELLOW, OUTPUT); digitalWrite(Config::LED_YELLOW, LOW);
  }
  pinMode(Config::MOTOR_IN1, OUTPUT); pinMode(Config::MOTOR_IN2, OUTPUT);
  digitalWrite(Config::MOTOR_IN1, LOW); digitalWrite(Config::MOTOR_IN2, LOW);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  motorOk = ledcAttach(Config::MOTOR_IN1, Config::PWM_HZ, Config::PWM_BITS);
#else
  motorOk = ledcSetup(0, Config::PWM_HZ, Config::PWM_BITS) > 0;
  if (motorOk) ledcAttachPin(Config::MOTOR_IN1, 0);
#endif
  aplicarMotor();
  pinMode(Config::POT_ADC, INPUT);
  analogReadResolution(12); analogSetPinAttenuation(Config::POT_ADC, ADC_11db);
  if (Config::BATTERY_ENABLED) {
    pinMode(Config::BATTERY_ADC, INPUT);
    analogReadResolution(12); analogSetPinAttenuation(Config::BATTERY_ADC, ADC_11db);
  }
  Wire.begin(Config::SDA, Config::SCL); Wire.setClock(400000);
  telaOk = tela.begin(SSD1306_SWITCHCAPVCC, Config::OLED_ADDRESS); tela.setTextWrap(false);
  carregarMemoria();
  paraEnviar = xQueueCreate(1, sizeof(Simples::Resumo)); resultadoEnvio = xQueueCreate(1, sizeof(bool));
  mensagensRede = xQueueCreate(1, sizeof(Simples::AvisoRede));
  tarefaRedeOk = paraEnviar && resultadoEnvio && mensagensRede &&
    xTaskCreatePinnedToCore(tarefaRede, "api", 12288, nullptr, 1, nullptr, 0) == pdPASS;
  if (!tarefaRedeOk) {
    estadoRede.estado = Simples::EstadoApi::Falha;
    strlcpy(mensagemApi, "ERRO tarefa rede", sizeof(mensagemApi));
  }
  ultimaAmostra = agoraMs(); mediaVazao.adicionar(0, ultimaAmostra);
  janelas.iniciar(ultimaAmostra * 1000);
  inicioTesteLocalMs = ultimaAmostra;
  memoria.inicioTesteMs = utcDe(inicioTesteLocalMs);
  zeroSemHora = !memoria.inicioTesteMs;
  memoriaAlterada = true;
  const esp_err_t erroHall = Simples::iniciarHallInterrupt(static_cast<gpio_num_t>(Config::HALL), pulsoHall, nullptr);
  hallOk = erroHall == ESP_OK;
  if (!hallOk) {
    Serial.printf("[HALL] Falha ao iniciar interrupcao: %s\n", esp_err_to_name(erroHall));
    if (erroHall == ESP_ERR_INVALID_STATE)
      Serial.println("[HALL] Servico GPIO preexistente; flag IRAM nao verificavel. Motor bloqueado.");
  }
  protegerRegistros();
  Serial.println("Potenciometro: Desligado | Modo normal | Modo anomalo. Ao iniciar, passe por Desligado.");
  Serial.println("Freq.est = referencia de rotacao do modo. Litros/vazao = pulsos reais do Hall.");
  Serial.printf("LEDs de modo: %s (Config.h).\n", Config::LEDS_ENABLED ? "habilitados" : "desativados");
  Serial.println("Serial: ZERAR | STATUS | TESTE (115200, Nova linha). Reiniciar limpa fila e total do teste.");
}
void loop() {
  cuidarEnvios(); fecharJanela(); protegerRegistros();
  lerPotenciometro(); lerSerial(); atualizarMedidas();
  if (mensagensRede && xQueueReceive(mensagensRede, &estadoRede, 0) == pdTRUE) {
    strlcpy(mensagemApi, estadoRede.texto, sizeof(mensagemApi));
  }
  atualizarTela(); delay(2);
}

#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include "HttpBodyDecoder.h"

namespace HttpBody {
struct Resultado {
  Estado estado;
  size_t bytes;
  constexpr bool completo() const { return estado == Estado::Completo; }
};
constexpr const char* nomeEstado(Estado estado) {
  switch (estado) {
    case Estado::Completo: return "completo";
    case Estado::Limite: return "limite";
    case Estado::Timeout: return "timeout";
    case Estado::Erro: return "erro";
    default: return "lendo";
  }
}

// Coletar Transfer-Encoding no HTTPClient antes de fazer a requisicao.
// Prazo TOTAL para o corpo, contado depois de receber os cabecalhos HTTP.
inline Resultado lerCorpoHttp(HTTPClient& http, String& corpo, size_t limiteBytes, uint32_t prazoTotalMs) {
  corpo = "";
  String encoding = http.header("Transfer-Encoding");
  encoding.trim(); encoding.toLowerCase();
  if (encoding.length() && encoding != "identity" && encoding != "chunked") return {Estado::Erro, 0};
  Decoder decoder(http.getSize(), encoding == "chunked", limiteBytes);
  if (decoder.estado() != Estado::Lendo) return {decoder.estado(), 0};
  NetworkClient* cliente = http.getStreamPtr();
  if (!cliente) { decoder.encerrar(); return {decoder.estado(), 0}; }
  const uint32_t inicio = millis();
  uint8_t entrada[256];
  char saida[256];
  while (decoder.estado() == Estado::Lendo) {
    if (static_cast<uint32_t>(millis() - inicio) >= prazoTotalMs)
      return {Estado::Timeout, corpo.length()};
    const int disponiveis = cliente->available();
    if (disponiveis < 0) return {Estado::Erro, corpo.length()};
    if (disponiveis > 0) {
      const size_t pedido = static_cast<size_t>(disponiveis) < sizeof(entrada) ?
        static_cast<size_t>(disponiveis) : sizeof(entrada);
      const int lidos = cliente->read(entrada, pedido); // So bytes ja disponiveis; nunca readBytes.
      if (lidos < 0) return {Estado::Erro, corpo.length()};
      size_t n = 0;
      for (int i = 0; i < lidos; ++i) {
        if (decoder.receber(entrada[i])) saida[n++] = static_cast<char>(entrada[i]);
        if (decoder.estado() != Estado::Lendo) break;
      }
      if (n && !corpo.concat(saida, n)) return {Estado::Erro, corpo.length()};
    } else if (!cliente->connected()) decoder.encerrar();
    delay(1); // Libera Wi-Fi/watchdog enquanto ha progresso e durante espera.
  }
  return {decoder.estado(), corpo.length()};
}
}

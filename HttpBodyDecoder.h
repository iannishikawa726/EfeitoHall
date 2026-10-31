#pragma once
#include <stddef.h>
#include <stdint.h>

namespace HttpBody {
enum class Estado : uint8_t { Lendo, Completo, Limite, Timeout, Erro };

// Decoder incremental. Nunca aloca memoria nem espera por dados.
class Decoder {
  enum class Fase : uint8_t { Direto, Tamanho, Dados, DadosCR, DadosLF, Trailer };
  Estado situacao = Estado::Lendo;
  Fase fase;
  size_t limite, recebidos = 0, restantes = 0;
  size_t framing = 0, trailers = 0, usados = 0;
  bool fechamento, aguardandoLF = false;
  char linha[257]{};

  static constexpr bool token(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') || c == '!' || c == '#' || c == '$' ||
      c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
      c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
  }
  static constexpr int hexadecimal(uint8_t c) {
    return c >= '0' && c <= '9' ? c - '0' :
      c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  }
  constexpr void terminarLinha() {
    if (fase == Fase::Trailer) {
      if (!usados) { situacao = Estado::Completo; return; }
      size_t i = 0;
      while (i < usados && token(static_cast<uint8_t>(linha[i]))) ++i;
      if (!i || i == usados || linha[i] != ':') { situacao = Estado::Erro; return; }
      for (++i; i < usados; ++i) {
        const uint8_t c = static_cast<uint8_t>(linha[i]);
        if ((c < 32 && c != '\t') || c == 127) { situacao = Estado::Erro; return; }
      }
    } else {
      size_t i = 0, tamanho = 0;
      for (; i < usados; ++i) {
        const int digito = hexadecimal(static_cast<uint8_t>(linha[i]));
        if (digito < 0) break;
        const size_t disponivel = limite - recebidos;
        if (tamanho > disponivel / 16 ||
            (tamanho == disponivel / 16 && static_cast<size_t>(digito) > disponivel % 16)) {
          situacao = Estado::Limite; return;
        }
        tamanho = tamanho * 16 + static_cast<size_t>(digito);
      }
      if (!i) { situacao = Estado::Erro; return; }
      while (i < usados && (linha[i] == ' ' || linha[i] == '\t')) ++i;
      if (i < usados && linha[i] != ';') { situacao = Estado::Erro; return; }
      // Extensoes sao ignoradas, mas sua linha tem tamanho/control characters limitados.
      for (; i < usados; ++i) {
        const uint8_t c = static_cast<uint8_t>(linha[i]);
        if ((c < 32 && c != '\t') || c == 127) { situacao = Estado::Erro; return; }
      }
      restantes = tamanho;
      fase = tamanho ? Fase::Dados : Fase::Trailer;
    }
    usados = 0;
  }
public:
  // tamanho < 0 indica corpo ate fechar; chunked tem precedencia sobre Content-Length.
  constexpr Decoder(int tamanho, bool chunked, size_t maximo)
    : fase(chunked ? Fase::Tamanho : Fase::Direto), limite(maximo),
      restantes(tamanho > 0 ? static_cast<size_t>(tamanho) : 0),
      fechamento(!chunked && tamanho < 0) {
    if (!chunked && tamanho >= 0) {
      if (restantes > limite) situacao = Estado::Limite;
      else if (!restantes) situacao = Estado::Completo;
    }
  }
  constexpr Estado estado() const { return situacao; }
  constexpr size_t bytes() const { return recebidos; }
  constexpr void encerrar() {
    if (situacao == Estado::Lendo) situacao = fechamento ? Estado::Completo : Estado::Erro;
  }
  // true: este byte pertence ao corpo decodificado e deve ser entregue ao chamador.
  constexpr bool receber(uint8_t c) {
    if (situacao != Estado::Lendo) return false;
    if (fase == Fase::Direto || fase == Fase::Dados) {
      if (recebidos >= limite) { situacao = Estado::Limite; return false; }
      ++recebidos;
      if (fase == Fase::Dados) {
        if (!--restantes) fase = Fase::DadosCR;
      } else if (!fechamento && !--restantes) situacao = Estado::Completo;
      return true;
    }
    if (++framing > 8192 || (fase == Fase::Trailer && ++trailers > 2048)) {
      situacao = Estado::Limite; return false;
    }
    if (fase == Fase::DadosCR) {
      if (c != '\r') situacao = Estado::Erro;
      else fase = Fase::DadosLF;
    } else if (fase == Fase::DadosLF) {
      if (c != '\n') situacao = Estado::Erro;
      else fase = Fase::Tamanho;
    } else if (aguardandoLF) {
      aguardandoLF = false;
      if (c != '\n') situacao = Estado::Erro;
      else terminarLinha();
    } else if (c == '\r') aguardandoLF = true;
    else if (c == '\n') situacao = Estado::Erro;
    else if (usados >= sizeof(linha) - 1) situacao = Estado::Limite;
    else linha[usados++] = static_cast<char>(c);
    return false;
  }
};
}

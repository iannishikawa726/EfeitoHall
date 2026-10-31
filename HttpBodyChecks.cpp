#include "HttpBodyDecoder.h"

namespace TestesCorpoHttp {
using HttpBody::Decoder;
using HttpBody::Estado;
struct Leitura { Estado estado; size_t bytes; char corpo[64]{}; };
constexpr Leitura ler(const char* entrada, int tamanho, bool chunked, size_t limite, bool fechar) {
  Decoder d(tamanho, chunked, limite);
  Leitura r{d.estado(), 0, {}};
  for (size_t i = 0; entrada[i] && d.estado() == Estado::Lendo; ++i)
    if (d.receber(static_cast<uint8_t>(entrada[i])) && r.bytes < sizeof(r.corpo) - 1)
      r.corpo[r.bytes++] = entrada[i];
  if (fechar) d.encerrar();
  r.estado = d.estado();
  return r;
}
constexpr bool igual(const char* a, const char* b) {
  for (size_t i = 0; ; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
}
static_assert(ler("abcEXTRA", 3, false, 3, false).estado == Estado::Completo &&
  igual(ler("abcEXTRA", 3, false, 3, false).corpo, "abc"), "Content-Length termina sem esperar fechamento");
static_assert(ler("", 0, false, 0, false).estado == Estado::Completo,
  "Corpo vazio nao espera dados nem conexao");
static_assert(ler("abc", 4, false, 10, true).estado == Estado::Erro,
  "Fechamento antes de Content-Length e erro");
static_assert(ler("abcdef", -1, false, 10, true).estado == Estado::Completo &&
  ler("abcdef", -1, false, 10, false).estado == Estado::Lendo,
  "Corpo sem tamanho termina somente no fechamento");
static_assert(ler("abcdef", -1, false, 3, true).estado == Estado::Limite &&
  igual(ler("abcdef", -1, false, 3, true).corpo, "abc"), "Limite nao permite anexar um byte extra");
static_assert(ler("", 100, false, 10, false).estado == Estado::Limite,
  "Content-Length maior que limite e recusado antes de alocar corpo");
static_assert(ler("3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", -1, true, 5, false).estado == Estado::Completo &&
  igual(ler("3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n", -1, true, 5, false).corpo, "abcde"),
  "Chunked remove enquadramento e aceita limite exato");
static_assert(ler("A;foo=bar\r\n0123456789\r\n0\r\nX-Test: ok\r\n\r\n", 999, true, 10, false).estado == Estado::Completo,
  "Hexadecimal maiusculo, extensao e trailer; chunked tem precedencia sobre Content-Length");
static_assert(ler("6\r\nabcdef\r\n0\r\n\r\n", -1, true, 5, false).estado == Estado::Limite,
  "Tamanho do chunk excedente e recusado antes de receber seu payload");
static_assert(ler("z\r\n", -1, true, 10, false).estado == Estado::Erro &&
  ler("1\na", -1, true, 10, false).estado == Estado::Erro &&
  ler("1\r\naX", -1, true, 10, false).estado == Estado::Erro,
  "Hexadecimal invalido e CRLF ausentes sao erros");
static_assert(ler("0\r\ninvalid trailer\r\n\r\n", -1, true, 10, false).estado == Estado::Erro,
  "Trailer precisa ter nome de campo e dois-pontos");
static_assert(ler("1\r\na\r\n0\r\n", -1, true, 10, true).estado == Estado::Erro,
  "Chunked truncado nao e confirmado como completo");
constexpr bool linhaLonga() {
  Decoder d(-1, true, 32768);
  d.receber('0'); d.receber(';');
  for (int i = 0; i < 255; ++i) d.receber('x');
  return d.estado() == Estado::Limite;
}
static_assert(linhaLonga(), "Extensao excessiva respeita limite de 256 bytes por linha");
constexpr bool trailersLongos() {
  Decoder d(-1, true, 32768);
  const char* inicio = "0\r\n";
  for (int i = 0; inicio[i]; ++i) d.receber(inicio[i]);
  for (int i = 0; i < 400; ++i) {
    const char* linha = "X: a\r\n";
    for (int j = 0; linha[j]; ++j) d.receber(linha[j]);
  }
  return d.estado() == Estado::Limite;
}
static_assert(trailersLongos(), "Trailers totais nao excedem 2048 bytes");
constexpr bool framingLongo() {
  Decoder d(-1, true, 32768);
  for (int i = 0; i < 2000; ++i) {
    const char* bloco = "1\r\na\r\n";
    for (int j = 0; bloco[j]; ++j) d.receber(bloco[j]);
  }
  return d.estado() == Estado::Limite && d.bytes() < 32768;
}
static_assert(framingLongo(), "Muitos chunks minimos tambem respeitam limite total de enquadramento");
static_assert(ler("abc", -1, false, 3, true).estado == Estado::Completo &&
  ler("0\r\n\r\n", -1, true, 0, false).estado == Estado::Completo,
  "Limite exato nao acusa excesso no fechamento ou no chunk vazio final");
}

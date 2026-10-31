#pragma once
#include <stdint.h>
#include <stddef.h>
#include <float.h>
#include <string.h>

namespace Simples {
enum class EstadoApi : uint8_t { Aguardando, Enviando, Confirmada, Falha };
struct AvisoRede {
  EstadoApi estado;
  int codigo;
  char texto[24];
};
constexpr double frequenciaEstimada(uint8_t modo, double normal, double anomalo) {
  if (!modo) return 0;
  const double valor = modo == 1 ? normal : modo == 2 ? anomalo : -1;
  return valor > 0 && valor <= DBL_MAX ? valor : -1;
}

// Exige repouso continuo. Qualquer saida do minimo ou comando ativo reinicia a espera.
class RepousoBateria {
  int64_t espera, desde = -1;
public:
  constexpr explicit RepousoBateria(int64_t ms) : espera(ms) {}
  constexpr void observar(bool paradoNoMinimo, int64_t ms) {
    if (!paradoNoMinimo) desde = -1;
    else if (desde < 0) desde = ms;
  }
  constexpr bool pronto(int64_t ms) const {
    return desde >= 0 && ms >= desde && ms - desde >= espera;
  }
};

// Recebe uma amostra por segundo ENQUANTO EM REPOUSO. Suaviza em passos de 5%
// e exige 2 pontos percentuais alem do meio do passo para evitar oscilacoes.
class IndicadorBateria {
  double vazia, cheia, filtrada = -1;
  int percentual = -1;
  int64_t ultimaMudanca = 0;
public:
  constexpr IndicadorBateria(double v0, double v100) : vazia(v0), cheia(v100) {}
  constexpr int porcentagem() const { return percentual; } // -1 = sem leitura.
  constexpr double tensaoFiltrada() const { return filtrada; }
  constexpr void atualizar(double v, int64_t ms) {
    if (!(v >= 0 && v <= DBL_MAX && vazia >= 0 && cheia > vazia && cheia <= DBL_MAX)) {
      filtrada = -1; percentual = -1; return;
    }
    filtrada = filtrada < 0 ? v : filtrada * 0.9 + v * 0.1;
    double p = 100.0 * (filtrada - vazia) / (cheia - vazia);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    const int arredondado = static_cast<int>((p + 2.5) / 5) * 5;
    if (percentual < 0) {
      percentual = arredondado; ultimaMudanca = ms;
    } else if (ms - ultimaMudanca >= 5000 &&
               (p >= percentual + 4.5 || p <= percentual - 4.5)) {
      // No maximo um passo por cinco segundos, mesmo numa queda brusca de tensao.
      percentual += arredondado > percentual ? 5 : -5;
      ultimaMudanca = ms;
    }
  }
};

// Tres faixas com histerese e confirmacao temporal. Exige OFF depois de um bloqueio.
class ControlePotenciometro {
  uint16_t normal, anomalo, histerese;
  int64_t estabilidadeMs, desde = 0, zeroDesde = -1;
  uint8_t modo = 0, candidato = 0;
  bool armado = false;
public:
  constexpr ControlePotenciometro(uint16_t n, uint16_t a, uint16_t h, int64_t tempo)
    : normal(n), anomalo(a), histerese(h), estabilidadeMs(tempo) {}
  constexpr void bloquear() { modo = candidato = 0; armado = false; zeroDesde = -1; }
  constexpr bool habilitado() const { return armado; }
  constexpr uint8_t atualizar(uint16_t adc, int64_t ms, bool bloqueado) {
    if (bloqueado) { bloquear(); return 0; }
    if (!armado) {
      if (adc >= normal - histerese) zeroDesde = -1;
      else {
        if (zeroDesde < 0) zeroDesde = ms;
        if (ms - zeroDesde >= estabilidadeMs) { armado = true; desde = ms; }
      }
      return 0;
    }
    uint8_t novo = modo;
    if (modo == 0) {
      if (adc >= anomalo + histerese) novo = 2;
      else if (adc >= normal + histerese) novo = 1;
    } else if (modo == 1) {
      if (adc < normal - histerese) novo = 0;
      else if (adc >= anomalo + histerese) novo = 2;
    } else {
      if (adc < normal - histerese) novo = 0;
      else if (adc < anomalo - histerese) novo = 1;
    }
    if (novo != candidato) { candidato = novo; desde = ms; }
    if (ms - desde >= estabilidadeMs) modo = candidato;
    return modo;
  }
};

constexpr uint32_t FORMATO = 0x48533101;
constexpr size_t LIMITE_FILA = 24;
constexpr int64_t JANELA_MS = 300000;
constexpr bool devePausar(size_t fila, int64_t atrasadas, uint64_t excedentes,
                         bool bloqueada, bool falhaGravacao) {
  return bloqueada || falhaGravacao || excedentes || fila >= LIMITE_FILA - 1 ||
         atrasadas >= static_cast<int64_t>(LIMITE_FILA - 1);
}
// Categorias do validador MongoDB. Os limiares em dBm sao escolhas deste firmware.
constexpr const char* sinalWifi(int rssi) {
  return rssi >= -50 ? "excellent" : rssi >= -60 ? "good" : rssi >= -75 ? "weak" : "critical";
}
struct Resumo { int64_t inicioMs, fimMs; double litros; };
struct Memoria {
  uint32_t formato;
  uint32_t quantidade;
  char dispositivo[104];
  double totalLitros, zeroLitros;
  int64_t inicioTesteMs;
  Resumo fila[LIMITE_FILA];
};
// Cada ligacao inicia um teste independente, sem reutilizar fila ou totais salvos.
constexpr bool novaSessao(Memoria& m, const char* dispositivo) {
  Memoria limpa{};
  limpa.formato = FORMATO;
  size_t i = 0;
  for (; i + 1 < sizeof(limpa.dispositivo) && dispositivo[i]; ++i)
    limpa.dispositivo[i] = dispositivo[i];
  if (dispositivo[i]) return false;
  m = limpa;
  return true;
}
constexpr double litrosDosPulsos(uint64_t pulsos, double pulsosPorLitro) {
  return pulsosPorLitro > 0 ? pulsos / pulsosPorLitro : 0;
}
constexpr bool resumoValido(const Resumo& r) {
  return r.inicioMs >= 1704067200000LL && r.fimMs > r.inicioMs &&
         r.litros >= 0 && r.litros <= DBL_MAX;
}
// Aceita tambem o pequeno atraso de fechamento da versao anterior (ate 1 s).
constexpr bool janelaCompativel(const Resumo& r) {
  return resumoValido(r) && r.fimMs - r.inicioMs >= JANELA_MS - 1000 &&
         r.fimMs - r.inicioMs <= JANELA_MS + 1000;
}
constexpr double mediaLpm(const Resumo& r) { return r.litros * 60000.0 / (r.fimMs - r.inicioMs); }
constexpr bool adicionar(Memoria& m, const Resumo& r) {
  if (!resumoValido(r) || m.quantidade >= LIMITE_FILA ||
      (m.quantidade && r.inicioMs < m.fila[m.quantidade - 1].fimMs)) return false;
  m.fila[m.quantidade++] = r;
  return true;
}
constexpr void removerPrimeiro(Memoria& m) {
  if (!m.quantidade) return;
  --m.quantidade;
  for (size_t i = 0; i < m.quantidade; ++i) m.fila[i] = m.fila[i + 1];
  m.fila[m.quantidade] = {};
}
inline bool memoriaValida(const Memoria& m, const char* dispositivo) {
  if (m.formato != FORMATO || m.quantidade > LIMITE_FILA || m.dispositivo[103] ||
      strcmp(m.dispositivo, dispositivo) || !(m.totalLitros >= 0 && m.totalLitros <= DBL_MAX) ||
      !(m.zeroLitros >= 0 && m.zeroLitros <= DBL_MAX) || m.inicioTesteMs < 0) return false;
  for (size_t i = 0; i < m.quantidade; ++i)
    if (!resumoValido(m.fila[i]) || (i && m.fila[i].inicioMs < m.fila[i - 1].fimMs)) return false;
  return true;
}

// Contagens por janela, independentes da demora do loop, do NTP e da API.
// Guarda somente janelas com pulsos; janelas vazias nao ocupam este buffer.
// O loop e a ISR usam esta classe sob o mesmo hallMux.
class JanelasFixas {
  struct Contagem { int64_t numero = -1; uint64_t pulsos = 0; } itens[LIMITE_FILA + 1]{};
  int64_t atual = 0, proxima = 0, inicioAtualUs = 0, inicioProximaUs = 0;
  int ultimo = -1;
  uint64_t semEspaco = 0;
public:
  constexpr void iniciar(int64_t us) { inicioAtualUs = inicioProximaUs = us; }
  __attribute__((always_inline)) constexpr void avancar(int64_t us) {
    while (us >= inicioAtualUs + JANELA_MS * 1000) {
      inicioAtualUs += JANELA_MS * 1000;
      ++atual;
    }
  }
  __attribute__((always_inline)) constexpr void contar(int64_t us) {
    avancar(us);
    if (ultimo >= 0 && itens[ultimo].numero == atual) { ++itens[ultimo].pulsos; return; }
    for (size_t i = 0; i < LIMITE_FILA + 1; ++i) {
      if (itens[i].numero == atual || itens[i].numero < 0) {
        itens[i].numero = atual; ++itens[i].pulsos; ultimo = static_cast<int>(i); return;
      }
    }
    ++semEspaco; // Nunca sobrescreve registros antigos; sinaliza movimento alem da capacidade.
  }
  constexpr int64_t atrasadas() const { return atual - proxima; }
  constexpr uint64_t excedentes() const { return semEspaco; }
  constexpr int64_t proximoInicioCompletoMs() const {
    return (inicioAtualUs + JANELA_MS * 1000) / 1000;
  }
  constexpr bool consultar(int64_t& inicioMs, uint64_t& n) const {
    if (proxima >= atual) return false;
    inicioMs = inicioProximaUs / 1000; n = 0;
    for (const auto& item : itens) if (item.numero == proxima) { n = item.pulsos; break; }
    return true;
  }
  constexpr void confirmar() {
    if (proxima >= atual) return;
    for (size_t i = 0; i < LIMITE_FILA + 1; ++i) if (itens[i].numero == proxima) {
      itens[i] = {};
      if (ultimo == static_cast<int>(i)) ultimo = -1;
      break;
    }
    ++proxima; inicioProximaUs += JANELA_MS * 1000;
  }
};

// Mesma regra do sketch Hall que funcionava: toda FALLING renova o intervalo.
class HallFilter {
  int64_t minimoUs, ultimaUs = 0;
  uint32_t divisor, fase = 0;
  uint64_t validas = 0;
public:
  constexpr HallFilter(int64_t minimo, uint32_t div) : minimoUs(minimo), divisor(div) {}
  __attribute__((always_inline)) constexpr bool edge(int64_t agora) {
    const int64_t intervalo = agora - ultimaUs;
    ultimaUs = agora;
    if (intervalo < minimoUs) return false;
    ++validas;
    if (++fase < divisor) return false;
    fase = 0;
    return true;
  }
  constexpr uint64_t validEdges() const { return validas; }
};

// Vazao pelos pulsos ABSOLUTOS e tempo real; independe do modo ou da frequencia estimada.
class MediaVazao {
  struct Amostra { uint64_t pulsos; int64_t ms; } amostras[4]{};
  size_t n = 0;
public:
  constexpr void adicionar(uint64_t pulsos, int64_t ms) {
    if (n && (ms <= amostras[n - 1].ms || pulsos < amostras[n - 1].pulsos)) return;
    if (n == 4) { for (size_t i = 0; i < 3; ++i) amostras[i] = amostras[i + 1]; --n; }
    amostras[n++] = {pulsos, ms};
  }
  constexpr double litrosPorMinuto(double pulsosPorLitro) const {
    return n < 2 || pulsosPorLitro <= 0 ? 0 :
      (amostras[n - 1].pulsos - amostras[0].pulsos) * 60000.0 /
      ((amostras[n - 1].ms - amostras[0].ms) * pulsosPorLitro);
  }
};
}

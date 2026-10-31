#include "Logica.h"

namespace TestesNovaSessao {
constexpr int64_t ANTIGO = 1792623600000LL;
constexpr bool limpaFilaETotal() {
  Simples::Memoria m{};
  m.totalLitros = 23.6; m.zeroLitros = 5; m.inicioTesteMs = ANTIGO;
  if (!Simples::adicionar(m, {ANTIGO, ANTIGO + 300000, 23.6})) return false;
  if (!Simples::novaSessao(m, "ESP32-SP-0156")) return false;
  if (m.formato != Simples::FORMATO || m.quantidade || m.totalLitros || m.zeroLitros ||
      m.inicioTesteMs || strcmp(m.dispositivo, "ESP32-SP-0156")) return false;
  for (const auto& r : m.fila) if (r.inicioMs || r.fimMs || r.litros) return false;
  return true;
}
static_assert(limpaFilaETotal(), "Reiniciar descarta 23.600 L salvos, referencia antiga e todas as janelas pendentes");
constexpr bool somenteJanelaNova() {
  Simples::Memoria m{};
  Simples::adicionar(m, {ANTIGO, ANTIGO + 300000, 0});
  if (!Simples::novaSessao(m, "ESP32-SP-0156")) return false;
  return Simples::adicionar(m, {ANTIGO + 86400000, ANTIGO + 86700000, 2}) &&
    m.quantidade == 1 && m.fila[0].inicioMs == ANTIGO + 86400000 &&
    Simples::mediaLpm(m.fila[0]) == 0.4;
}
static_assert(somenteJanelaNova(), "Fila limpa aceita somente a nova janela, sem reenviar o cabecalho antigo");
static_assert(Simples::litrosDosPulsos(0, 10) == 0 && Simples::litrosDosPulsos(20, 10) == 2,
  "Total comeca em zero e cresce apenas com os pulsos da ligacao atual");
}

namespace TestesFila {
constexpr int64_t BASE = 1792623600000LL;
constexpr Simples::Resumo A{BASE, BASE + 300000, 10};
constexpr Simples::Resumo B{BASE + 300000, BASE + 600000, 20};
static_assert(Simples::resumoValido(A) && Simples::mediaLpm(A) == 2, "10 L em 5 min = 2 L/min");
static_assert(!Simples::resumoValido({BASE, BASE, 1}), "Nao aceitar duracao zero");
static_assert(!Simples::resumoValido({BASE, BASE + 1, -1}), "Nao aceitar volume negativo");
constexpr bool filaCheia() {
  Simples::Memoria m{};
  for (size_t i = 0; i < Simples::LIMITE_FILA; ++i)
    if (!Simples::adicionar(m, {BASE + static_cast<int64_t>(i) * 1000,
      BASE + static_cast<int64_t>(i + 1) * 1000, 1})) return false;
  const auto primeiro = m.fila[0];
  if (Simples::adicionar(m, {m.fila[m.quantidade - 1].fimMs,
      m.fila[m.quantidade - 1].fimMs + 1000, 2})) return false;
  return m.quantidade == Simples::LIMITE_FILA && m.fila[0].inicioMs == primeiro.inicioMs;
}
static_assert(filaCheia(), "Fila cheia recusa nova janela sem sobrescrever os pendentes");
constexpr bool ordem() {
  Simples::Memoria m{};
  if (!Simples::adicionar(m, A) || !Simples::adicionar(m, B)) return false;
  Simples::removerPrimeiro(m);
  return m.quantidade == 1 && m.fila[0].inicioMs == B.inicioMs && m.fila[0].litros == 20;
}
static_assert(ordem(), "Confirmacao remove somente o primeiro resumo");
constexpr bool sobreposicao() {
  Simples::Memoria m{};
  return Simples::adicionar(m, A) && !Simples::adicionar(m, {BASE + 1, BASE + 300001, 1}) && m.quantidade == 1;
}
static_assert(sobreposicao(), "Nao misturar janelas sobrepostas");
static_assert(!Simples::janelaCompativel({BASE, BASE + 500, 0.5}) &&
  !Simples::janelaCompativel({BASE, BASE + 600000, 2}) && Simples::janelaCompativel(A),
  "Janelas parciais ou acumuladas por dez minutos nao seguem para o detector");
constexpr bool mediaReal() {
  Simples::MediaVazao f;
  f.adicionar(0, 0); f.adicionar(9, 900); f.adicionar(22, 2200); f.adicionar(30, 3000);
  return f.litrosPorMinuto(10) == 60;
}
static_assert(mediaReal(), "30 pulsos em 3 s a 10 pulsos/L resultam em 60 L/min, sem usar frequencia estimada");
constexpr bool parada() {
  Simples::MediaVazao f;
  f.adicionar(0, 0); f.adicionar(10, 1000); f.adicionar(10, 2000); f.adicionar(10, 3000); f.adicionar(10, 4000);
  return f.litrosPorMinuto(10) == 0;
}

namespace TestesPotenciometro {
using Simples::ControlePotenciometro;
constexpr bool partidaDesligada() {
  ControlePotenciometro p(1365, 2730, 80, 150);
  if (p.atualizar(4095, 0, false) || p.atualizar(4095, 10000, false) || p.habilitado()) return false;
  p.atualizar(0, 10020, false); p.atualizar(0, 10169, false);
  if (p.habilitado()) return false;
  return !p.atualizar(0, 10170, false) && p.habilitado();
}
static_assert(partidaDesligada(), "Ligar a placa com potenciometro alto nao aciona o motor; exige OFF estavel");
constexpr bool tresModos() {
  ControlePotenciometro p(1365, 2730, 80, 150);
  p.atualizar(0, 0, false); p.atualizar(0, 150, false);
  if (p.atualizar(2000, 200, false) || p.atualizar(2000, 349, false)) return false;
  if (p.atualizar(2000, 350, false) != 1) return false;
  p.atualizar(4095, 400, false);
  if (p.atualizar(4095, 550, false) != 2) return false;
  p.atualizar(0, 600, false);
  return !p.atualizar(0, 750, false);
}
static_assert(tresModos(), "Faixas produzem desligado, normal e anomalo com confirmacao temporal");
constexpr bool histerese() {
  ControlePotenciometro p(1365, 2730, 80, 150);
  p.atualizar(0, 0, false); p.atualizar(0, 150, false);
  p.atualizar(2000, 200, false); p.atualizar(2000, 350, false);
  if (p.atualizar(1350, 500, false) != 1 || p.atualizar(1350, 700, false) != 1) return false;
  p.atualizar(4095, 750, false); p.atualizar(4095, 900, false);
  if (p.atualizar(2700, 950, false) != 2 || p.atualizar(2700, 1150, false) != 2) return false;
  p.atualizar(2600, 1200, false);
  return p.atualizar(2600, 1350, false) == 1;
}
static_assert(histerese(), "Ruido proximo aos dois limites nao fica alternando o modo");
constexpr bool pausaEZero() {
  ControlePotenciometro p(1365, 2730, 80, 150);
  p.atualizar(0, 0, false); p.atualizar(0, 150, false);
  p.atualizar(4095, 200, false);
  if (p.atualizar(4095, 350, false) != 2 || p.atualizar(4095, 400, true)) return false;
  if (p.atualizar(4095, 1000, false) || p.habilitado()) return false;
  p.atualizar(0, 1100, false); p.atualizar(0, 1250, false);
  p.atualizar(2000, 1300, false);
  if (p.atualizar(2000, 1450, false) != 1) return false;
  p.bloquear();
  return !p.habilitado() && !p.atualizar(2000, 2000, false);
}
static_assert(pausaEZero(), "Depois da pausa ou ZERAR, motor so volta apos passar por OFF");
static_assert(Simples::frequenciaEstimada(0, 5.0 / 17, 5.0 / 7) == 0 &&
  Simples::frequenciaEstimada(1, 5.0 / 17, 5.0 / 7) > 0.29 &&
  Simples::frequenciaEstimada(1, 5.0 / 17, 5.0 / 7) < 0.30 &&
  Simples::frequenciaEstimada(2, 5.0 / 17, 5.0 / 7) > 0.71 &&
  Simples::frequenciaEstimada(2, 5.0 / 17, 5.0 / 7) < 0.72,
  "Frequencias de rotacao usam apenas as referencias por modo, nao os pulsos Hall");
constexpr bool volumeIndependente() {
  Simples::MediaVazao v;
  v.adicionar(100, 0); v.adicionar(120, 2000);
  const double antes = v.litrosPorMinuto(10);
  const double normal = Simples::frequenciaEstimada(1, 0.3, 0.7);
  const double anomalo = Simples::frequenciaEstimada(2, 0.3, 0.7);
  return normal != anomalo && antes == 60 && v.litrosPorMinuto(10) == antes &&
    Simples::mediaLpm({TestesFila::BASE, TestesFila::BASE + 300000, 2}) == 0.4;
}
static_assert(volumeIndependente(), "Trocar frequencia de referencia nao altera litros e vazao por pulsos reais");
}
static_assert(parada(), "Media zera apos tres intervalos sem pulsos");
}

namespace TestesJanelas {
constexpr int64_t DURACAO = Simples::JANELA_MS * 1000;
constexpr bool fronteiraExata() {
  Simples::JanelasFixas j;
  j.iniciar(1000000);
  j.contar(1000000 + DURACAO - 1); j.contar(1000000 + DURACAO);
  int64_t inicio = 0; uint64_t n = 0;
  if (!j.consultar(inicio, n) || inicio != 1000 || n != 1) return false;
  j.confirmar(); j.avancar(1000000 + DURACAO * 2);
  return j.consultar(inicio, n) && inicio == 301000 && n == 1;
}
static_assert(fronteiraExata(), "Pulso na fronteira pertence a janela seguinte, mesmo com loop atrasado");
constexpr bool semFundir() {
  Simples::JanelasFixas j;
  j.iniciar(0); j.contar(10000); j.contar(DURACAO + 20000); j.avancar(DURACAO * 4);
  int64_t inicio = 0; uint64_t n = 0;
  for (int64_t i = 0; i < 4; ++i) {
    if (!j.consultar(inicio, n) || inicio != Simples::JANELA_MS * i || n != (i < 2 ? 1u : 0u)) return false;
    j.confirmar();
  }
  return !j.consultar(inicio, n);
}
static_assert(semFundir(), "Atraso de rede/NTP nao mistura quatro janelas em um resumo maior");
constexpr bool novoTeste() {
  Simples::JanelasFixas j;
  j.iniciar(0); j.contar(10000); j.avancar(DURACAO / 2);
  const int64_t inicioTeste = j.proximoInicioCompletoMs();
  j.contar(DURACAO / 2 + 10000); j.avancar(DURACAO);
  int64_t inicio = 0; uint64_t n = 0;
  return inicioTeste == Simples::JANELA_MS && j.consultar(inicio, n) && inicio == 0 && n == 2;
}
static_assert(novoTeste(), "Marcar novo teste nao corta a janela nem perde os pulsos anteriores");
constexpr bool pausaLonga() {
  Simples::JanelasFixas j;
  j.iniciar(0); j.contar(10000); j.avancar(DURACAO * 100);
  int64_t inicio = 0; uint64_t n = 0;
  if (!j.consultar(inicio, n) || n != 1) return false;
  j.confirmar();
  for (int i = 1; i < 100; ++i) {
    if (!j.consultar(inicio, n) || n) return false;
    j.confirmar();
  }
  j.contar(DURACAO * 100 + 1); j.avancar(DURACAO * 101);
  return j.consultar(inicio, n) && inicio == Simples::JANELA_MS * 100 && n == 1 && !j.excedentes();
}
static_assert(pausaLonga(), "Pausa longa sem movimento nao ocupa buffer com zeros nem desloca datas");
constexpr bool capacidade() {
  Simples::JanelasFixas j;
  j.iniciar(0);
  for (size_t i = 0; i < Simples::LIMITE_FILA + 2; ++i) j.contar(DURACAO * i + 10000);
  int64_t inicio = 0; uint64_t n = 0;
  if (j.excedentes() != 1 || !j.consultar(inicio, n) || n != 1) return false;
  j.confirmar(); j.contar(DURACAO * (Simples::LIMITE_FILA + 2) + 10000);
  return j.excedentes() == 1;
}
static_assert(capacidade(), "Movimento alem da capacidade sinaliza excedente sem sobrescrever o primeiro registro");
static_assert(!Simples::devePausar(22, 0, 0, false, false) &&
  Simples::devePausar(23, 0, 0, false, false) && Simples::devePausar(0, 23, 0, false, false) &&
  Simples::devePausar(0, 0, 0, true, false) && Simples::devePausar(0, 0, 0, false, true),
  "Motor para antes da ultima vaga e tambem diante de falha de memoria ou falta prolongada de horario");
static_assert(!strcmp(Simples::sinalWifi(-50), "excellent") && !strcmp(Simples::sinalWifi(-51), "good") &&
  !strcmp(Simples::sinalWifi(-60), "good") && !strcmp(Simples::sinalWifi(-61), "weak") &&
  !strcmp(Simples::sinalWifi(-75), "weak") && !strcmp(Simples::sinalWifi(-76), "critical"),
  "RSSI usa apenas categorias aceitas pelo MongoDB, inclusive nos limiares");
static_assert(Simples::janelaCompativel({TestesFila::BASE, TestesFila::BASE + 300999, 1}) &&
  !Simples::janelaCompativel({TestesFila::BASE, TestesFila::BASE + 301001, 1}),
  "Compativel com pequeno atraso antigo, sem admitir janelas prolongadas");
}

namespace {
using Simples::HallFilter;
constexpr bool rejectedRetriggers() {
  HallFilter h(3000, 1);
  if (!h.edge(10000) || h.edge(11000) || h.edge(13000)) return false;
  return h.edge(16000) && h.validEdges() == 2;
}
static_assert(rejectedRetriggers(), "Borda rejeitada tambem renova o tempo, como no sketch original");
constexpr bool exactThreshold() {
  HallFilter h(3000, 1);
  return !h.edge(2999) && !h.edge(5998) && h.edge(8998) && h.validEdges() == 1;
}
static_assert(exactThreshold(), "Aceitar intervalo de 3000 us, rejeitar 2999 us");
constexpr bool dividerSurvivesNoise() {
  HallFilter h(3000, 2);
  if (h.edge(10000) || h.edge(10100) || h.validEdges() != 1) return false;
  return h.edge(20000) && h.validEdges() == 2 && !h.edge(30000) && h.edge(40000);
}
static_assert(dividerSurvivesNoise(), "Ruido rejeitado nao entra no divisor; preservar par entre leituras");
constexpr bool doubledPasses() {
  HallFilter h(3000, 2);
  unsigned count = 0;
  for (int64_t pass = 0; pass < 20; ++pass) {
    if (h.edge(10000 + pass * 100000)) ++count;
    if (h.edge(15000 + pass * 100000)) ++count;
  }
  return count == 20 && h.validEdges() == 40;
}
static_assert(doubledPasses(), "20 passagens com duas bordas validas cada resultam em 20 pulsos");
constexpr bool singlePasses() {
  HallFilter one(3000, 1), two(3000, 2);
  unsigned countOne = 0, countTwo = 0;
  for (int64_t pass = 0; pass < 20; ++pass) {
    const int64_t us = 10000 + pass * 100000;
    if (one.edge(us)) ++countOne;
    if (two.edge(us)) ++countTwo;
  }
  return countOne == 20 && countTwo == 10;
}
static_assert(singlePasses(), "Se o sensor gerar uma borda por passagem, divisor correto e 1");
constexpr bool shortBurst() {
  HallFilter h(3000, 2);
  if (h.edge(10000)) return false;
  for (int64_t us = 10003; us < 16000; us += 3) if (h.edge(us)) return false;
  return h.edge(20000) && h.validEdges() == 2;
}
static_assert(shortBurst(), "Rajada retriggera sem somar centenas de pulsos no divisor");
constexpr bool longUptime() {
  HallFilter h(3000, 1);
  const int64_t t = static_cast<int64_t>(UINT32_MAX) - 1000;
  return h.edge(t) && !h.edge(t + 2000) && h.edge(t + 5000);
}
static_assert(longUptime(), "Usar uptime de 64 bits, inclusive acima de UINT32_MAX us");

constexpr int64_t GAPS[] = {0, 3, 199, 200, 2999, 3000, 3001, 5000, 166667};
// Oraculo: a condicao da ISR do arquivo fornecido pelo usuario, sem mudancas.
constexpr bool matchesProvidedSketch() {
  for (uint32_t divider = 1; divider <= 4; ++divider) {
    HallFilter h(3000, divider);
    int64_t now = 0, lastEdge = 0;
    uint32_t validEdges = 0, seed = 12345;
    for (unsigned i = 0; i < 1500; ++i) {
      seed = seed * 1664525u + 1013904223u;
      now += GAPS[seed % 9];
      const int64_t gap = now - lastEdge;
      lastEdge = now;
      const bool reference = gap >= 3000 && (++validEdges % divider) == 0;
      if (h.edge(now) != reference || h.validEdges() != validEdges) return false;
    }
  }
  return true;
}
static_assert(matchesProvidedSketch(), "Reproduzir o sketch fornecido em 6000 bordas com ruido e pausas");
}

namespace {
constexpr bool bateriaSemAlternancia() {
  Simples::IndicadorBateria b(4.0, 6.4);
  b.atualizar(5.8, 0); // 75%.
  for (int64_t ms = 1000; ms <= 240000; ms += 1000) {
    // Ruido atravessa o ponto onde um simples arredondamento alternaria 75/80.
    b.atualizar(ms % 2000 ? 5.8576 : 5.8624, ms);
    if (b.porcentagem() != 75) return false;
  }
  return true;
}
static_assert(bateriaSemAlternancia(), "Histerese e filtro evitam alternancia perto do limite entre passos");

constexpr bool bateriaAcompanhaQuedaERecuperacao() {
  Simples::IndicadorBateria b(4.0, 6.4);
  b.atualizar(6.5, 0);
  if (b.porcentagem() != 100) return false;
  int anterior = 100;
  int64_t mudouEm = 0;
  for (int64_t ms = 1000; ms <= 600000; ms += 1000) {
    b.atualizar(ms <= 300000 ? 3.9 : 6.5, ms);
    const int p = b.porcentagem();
    if (p < 0 || p > 100 || p % 5) return false;
    if (p != anterior) {
      const int delta = p - anterior;
      if ((delta != -5 && delta != 5) || ms - mudouEm < 5000) return false;
      mudouEm = ms;
    }
    if (ms == 300000 && p != 0) return false;
    anterior = p;
  }
  return b.porcentagem() == 100;
}
static_assert(bateriaAcompanhaQuedaERecuperacao(), "Indicador acompanha queda e recuperacao sem saltos nem sair de 0 a 100");

constexpr bool bateriaExigeRepousoContinuo() {
  Simples::RepousoBateria r(15000);
  if (r.pronto(20000)) return false;
  r.observar(true, 1000);
  if (r.pronto(15999) || !r.pronto(16000)) return false;
  r.observar(false, 16001); // Giro ou potenciometro fora do minimo interrompe.
  if (r.pronto(90000)) return false;
  r.observar(true, 17000);
  return !r.pronto(31999) && r.pronto(32000);
}
static_assert(bateriaExigeRepousoContinuo(), "Qualquer interrupcao exige nova espera de quinze segundos");

constexpr bool bateriaPreservaReferenciaDuranteGiro() {
  Simples::RepousoBateria r(15000);
  Simples::IndicadorBateria b(4.0, 6.4);
  r.observar(true, 0);
  if (!r.pronto(15000)) return false;
  b.atualizar(5.2, 15000);
  const int referencia = b.porcentagem();
  const double tensao = b.tensaoFiltrada();
  for (int64_t ms = 16000; ms <= 60000; ms += 1000) {
    r.observar(false, ms);
    if (r.pronto(ms)) b.atualizar(6.9, ms); // Sinal contaminado nao pode entrar.
  }
  if (b.porcentagem() != referencia || b.tensaoFiltrada() != tensao) return false;
  r.observar(true, 61000);
  for (int64_t ms = 62000; ms <= 120000; ms += 1000)
    if (r.pronto(ms)) b.atualizar(4.4, ms);
  return b.porcentagem() < referencia;
}
static_assert(bateriaPreservaReferenciaDuranteGiro(), "Giro preserva ultima referencia e novo repouso permite atualizacao");
}


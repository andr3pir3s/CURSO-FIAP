/*
 * FarmTech Solutions - Fase 2
 * Sistema de irrigação inteligente (simulado no Wokwi com ESP32)
 *
 * Cultura escolhida: SOJA (Glycine max)
 *
 * Substituições didáticas (o Wokwi não tem sensores agrícolas):
 *   - Botão verde N  (GPIO 25) -> nível de Nitrogênio  (true/false)
 *   - Botão verde P  (GPIO 26) -> nível de Fósforo     (true/false)
 *   - Botão verde K  (GPIO 27) -> nível de Potássio    (true/false)
 *   - LDR            (GPIO 34) -> pH do solo (escala 0 a 14)
 *   - DHT22          (GPIO 15) -> umidade do solo (%)
 *   - Relé azul      (GPIO 13) -> bomba d'água
 *   - LED azul       (GPIO  4) -> espelha o estado da bomba (indicador visual)
 *
 * Dados externos (Python / OpenWeather):
 *   pelo Monitor Serial, digite  R<mm>  (ex.: R4.5) para informar a chuva
 *   prevista nas próximas 24 h. Digite ? para ver os comandos.
 *
 * Sem bibliotecas externas: o DHT22 é lido pelo protocolo de 1 fio implementado
 * neste arquivo (não precisa de libraries.txt).
 *
 * Saída serial: uma linha legível + uma linha "CSV,..." por ciclo.
 * As linhas CSV alimentam a análise estatística em R.
 */

#include <Arduino.h>   // único include: nenhuma biblioteca externa é necessária

// ============================== PINAGEM ==============================
#define PIN_BTN_N   25
#define PIN_BTN_P   26
#define PIN_BTN_K   27
#define PIN_LDR     34   // entrada analógica (somente leitura no ESP32)
#define PIN_DHT     15
#define PIN_RELE    13
#define PIN_LED     4

// ===================== PARÂMETROS DA CULTURA (SOJA) ==================
// Valores didáticos baseados em faixas usuais da literatura agronômica
// (Embrapa Soja). Ajuste conforme a pesquisa do grupo.
const float PH_MIN          = 6.0;   // faixa ideal de pH da soja
const float PH_MAX          = 6.8;
const float UMID_LIGA       = 60.0;  // abaixo disso o solo está seco -> irrigar
const float UMID_DESLIGA    = 75.0;  // acima disso o solo está úmido -> parar (histerese)
const float UMID_CRITICA    = 30.0;  // abaixo disso: emergência hídrica
const float CHUVA_LIMITE_MM = 2.0;   // chuva prevista (24 h) que suspende a irrigação

// ======================== CONFIGURAÇÕES DO CIRCUITO ==================
const bool  RELE_ATIVO_ALTO   = true;  // se o relé do Wokwi atuar invertido, troque para false
const bool  LDR_INVERTIDO     = true;  // true: mais luz => pH maior; se o seu LDR se comportar
                                       // ao contrário no Wokwi, troque para false
const bool  BOTOES_TOGGLE     = true;  // true: cada clique alterna o nível (adequado/deficiente)
                                       // false: nível = botão pressionado naquele instante
const unsigned long DEBOUNCE_MS          = 40;
const unsigned long INTERVALO_LEITURA_MS = 2000;  // DHT22 só lê a cada >= 2 s

// ============================== ESTADO ===============================
portMUX_TYPE muxDHT = portMUX_INITIALIZER_UNLOCKED;  // protege a janela de tempo crítica do DHT22
const int MAX_FALHAS_DHT = 3;   // leituras inválidas seguidas até acionar o fail-safe
int   falhasDHT       = 0;
float umidadeManual   = NAN;   // depuração: comando U<valor> substitui a leitura do DHT22

struct Botao {
  uint8_t       pino;
  const char*   nome;
  bool          nivel;        // true = nutriente adequado
  bool          ultimoLido;   // HIGH = solto (INPUT_PULLUP)
  unsigned long tMudanca;
};

Botao botoes[3] = {
  {PIN_BTN_N, "N", false, HIGH, 0},
  {PIN_BTN_P, "P", false, HIGH, 0},
  {PIN_BTN_K, "K", false, HIGH, 0}
};

struct Decisao {
  bool        bomba;
  const char* motivo;
};

float  umidade        = NAN;   // última leitura válida do DHT22
float  chuvaPrevistaMm = 0.0;  // atualizada pelo Monitor Serial (dados da API)
bool   bombaLigada    = false;
unsigned long ultimaLeitura = 0;
String bufferSerial   = "";

// ============================== FUNÇÕES ==============================

// ================== DHT22 SEM BIBLIOTECA (protocolo de 1 fio) ==================
// 1) Mestre puxa a linha para LOW por >= 1 ms e solta.
// 2) Sensor responde: LOW ~80 us, HIGH ~80 us.
// 3) Sensor envia 40 bits. Cada bit = LOW ~50 us + HIGH (~27 us = 0 | ~70 us = 1).
// 4) Dados: umidade (16 bits, x10), temperatura (16 bits, x10), checksum (8 bits).

// Mede por quantos microssegundos a linha permanece em "nivel".
// Retorna -1 se estourar o tempo limite.
int32_t medirPulso(int nivel, uint32_t timeoutUs) {
  uint32_t t0 = micros();
  while (digitalRead(PIN_DHT) == nivel) {
    if (micros() - t0 > timeoutUs) return -1;
  }
  return (int32_t)(micros() - t0);
}

// Diagnóstico da última leitura (impresso no serial quando o DHT22 falha)
int     dhtEstagio  = 0;          // 0=ok 1=sem resposta 2/3=resposta incompleta 10+N=bit N 90=checksum 91=faixa
int32_t dhtUltBaixo = 0, dhtUltAlto = 0;
uint8_t dhtBruto[5] = {0, 0, 0, 0, 0};

// Lê a umidade relativa (%). Retorna false se a leitura for inválida.
bool lerDHT22(float &umid) {
  uint8_t d[5] = {0, 0, 0, 0, 0};
  dhtEstagio = 0;

  // Linha em repouso (HIGH via pull-up) antes de começar
  pinMode(PIN_DHT, INPUT_PULLUP);
  delay(1);

  // Sinal de início: LOW por 2 ms
  pinMode(PIN_DHT, OUTPUT);
  digitalWrite(PIN_DHT, LOW);
  delay(2);

  bool ok = true;
  portENTER_CRITICAL(&muxDHT);   // temporização precisa (~5 ms)
  // SOLTA a linha sem forçá-la a HIGH (forçar HIGH causa "nan" com DHT22 em ESP32)
  pinMode(PIN_DHT, INPUT_PULLUP);

  // Resposta do sensor (timeouts folgados de 1 ms)
  if (medirPulso(HIGH, 1000) < 0)      { ok = false; dhtEstagio = 1; }   // espera o sensor puxar a linha
  else if (medirPulso(LOW, 1000) < 0)  { ok = false; dhtEstagio = 2; }   // resposta: LOW ~80 us
  else if (medirPulso(HIGH, 1000) < 0) { ok = false; dhtEstagio = 3; }   // resposta: HIGH ~80 us

  for (int i = 0; ok && i < 40; i++) {
    int32_t baixo = medirPulso(LOW, 1000);    // ~50 us
    int32_t alto  = medirPulso(HIGH, 1000);   // 27 us (bit 0) ou 70 us (bit 1)
    if (baixo < 0 || alto < 0) { ok = false; dhtEstagio = 10 + i; break; }
    dhtUltBaixo = baixo;
    dhtUltAlto  = alto;
    d[i / 8] <<= 1;
    if (alto > baixo) d[i / 8] |= 1;
  }
  portEXIT_CRITICAL(&muxDHT);

  memcpy(dhtBruto, d, 5);
  if (!ok) return false;

  if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) { dhtEstagio = 90; return false; }   // checksum

  float h = ((d[0] << 8) | d[1]) / 10.0;
  if (h < 0.0 || h > 100.0) { dhtEstagio = 91; return false; }
  umid = h;
  return true;
}

// Lê o LDR e converte a leitura (0..4095) para a escala de pH (0..14).
float lerPH() {
  long soma = 0;
  for (int i = 0; i < 8; i++) {
    soma += analogRead(PIN_LDR);
    delay(2);
  }
  float fracao = (soma / 8.0) / 4095.0;
  if (LDR_INVERTIDO) fracao = 1.0 - fracao;
  return fracao * 14.0;
}

// Lê os botões com debounce. Retorna true se algum nível mudou.
bool lerBotoes() {
  bool mudou = false;
  unsigned long agora = millis();

  for (int i = 0; i < 3; i++) {
    bool lido = digitalRead(botoes[i].pino);
    if (lido != botoes[i].ultimoLido && (agora - botoes[i].tMudanca) > DEBOUNCE_MS) {
      botoes[i].tMudanca   = agora;
      botoes[i].ultimoLido = lido;
      bool alterou = false;

      if (BOTOES_TOGGLE) {
        if (lido == LOW) {                 // clique (borda de descida)
          botoes[i].nivel = !botoes[i].nivel;
          alterou = true;
        }
      } else {
        botoes[i].nivel = (lido == LOW);   // pressionado = true
        alterou = true;
      }

      if (alterou) {
        mudou = true;
        Serial.printf(">> Nivel de %s alterado para %s. Ajuste o LDR (pH) para simular o efeito no solo.\n",
                      botoes[i].nome, botoes[i].nivel ? "ADEQUADO" : "DEFICIENTE");
      }
    }
  }
  return mudou;
}

// Lógica de decisão da bomba (documentada no README).
// Prioridade: segurança do sensor > saturação > emergência > chuva > pH/NPK.
Decisao decidir(float umid, float ph, bool n, bool p, bool k,
                float chuvaMm, bool bombaAtual) {
  (void)n;  // soja fixa N biologicamente (Bradyrhizobium): N não bloqueia a irrigação

  // 1) Sensor com defeito -> fail-safe: bomba desligada
  if (isnan(umid)) return {false, "FALHA no DHT22: bomba desligada por seguranca"};

  // 2) Solo suficientemente úmido -> desliga
  if (umid >= UMID_DESLIGA) return {false, "Solo umido: irrigacao desnecessaria"};

  // 3) Zona de histerese: se a bomba estava desligada, só liga abaixo de UMID_LIGA
  if (!bombaAtual && umid >= UMID_LIGA) return {false, "Umidade adequada"};

  // A partir daqui o solo está seco (ou a bomba já estava irrigando)

  // 4) Emergência hídrica: irriga mesmo com restrições de pH/NPK e previsão de chuva
  if (umid < UMID_CRITICA) return {true, "EMERGENCIA: umidade critica, irrigando"};

  // 5) Previsão de chuva -> economiza água
  if (chuvaMm >= CHUVA_LIMITE_MM) return {false, "Chuva prevista: irrigacao suspensa"};

  // 6) Retorno agronômico: pH fora da faixa ou P/K deficientes limitam a absorção
  bool phOk = (ph >= PH_MIN && ph <= PH_MAX);
  if (!phOk)  return {false, "Solo seco, mas pH fora da faixa ideal: corrigir solo (calagem)"};
  if (!p || !k) return {false, "Solo seco, mas P/K deficientes: adubar antes de irrigar"};

  // 7) Tudo ok -> irriga
  return {true, "Solo seco com pH e P/K adequados: irrigando"};
}

void acionarBomba(bool ligar) {
  bombaLigada = ligar;
  bool nivelRele = RELE_ATIVO_ALTO ? ligar : !ligar;
  digitalWrite(PIN_RELE, nivelRele ? HIGH : LOW);
  digitalWrite(PIN_LED, ligar ? HIGH : LOW);
}

void imprimir(float ph, const Decisao& d) {
  bool n = botoes[0].nivel, p = botoes[1].nivel, k = botoes[2].nivel;
  unsigned long t = millis() / 1000;

  Serial.printf("[%5lus] Umid=%.1f%% pH=%.2f N=%d P=%d K=%d Chuva=%.1fmm | BOMBA %s | %s\n",
                t, umidade, ph, n, p, k, chuvaPrevistaMm,
                bombaLigada ? "LIGADA" : "DESLIGADA", d.motivo);

  // Linha CSV para a análise em R
  Serial.printf("CSV,%lu,%.1f,%.2f,%d,%d,%d,%.1f,%d\n",
                t, umidade, ph, n, p, k, chuvaPrevistaMm, bombaLigada ? 1 : 0);
}

// Um ciclo completo: lê sensores, decide e atua.
void ciclo(bool lerDHT) {
  if (lerDHT) {
    if (!isnan(umidadeManual)) {
      umidade = umidadeManual;     // modo de depuração (comando U)
    } else {
      float h;
      if (lerDHT22(h)) {
        umidade = h;
        falhasDHT = 0;
      } else {
        falhasDHT++;
        Serial.printf(">> DHT22 falhou: estagio=%d baixo=%ldus alto=%ldus bytes=%02X %02X %02X %02X %02X [%d/%d]\n",
                      dhtEstagio, (long)dhtUltBaixo, (long)dhtUltAlto,
                      dhtBruto[0], dhtBruto[1], dhtBruto[2], dhtBruto[3], dhtBruto[4],
                      falhasDHT, MAX_FALHAS_DHT);
        if (falhasDHT >= MAX_FALHAS_DHT) umidade = NAN;   // aciona o fail-safe da decisão
      }
    }
  }
  float ph = lerPH();

  Decisao d = decidir(umidade, ph,
                      botoes[0].nivel, botoes[1].nivel, botoes[2].nivel,
                      chuvaPrevistaMm, bombaLigada);
  acionarBomba(d.bomba);
  imprimir(ph, d);
}

void mostrarAjuda() {
  Serial.println("Comandos do Monitor Serial:");
  Serial.println("  R<mm>  define a chuva prevista (24h) em mm. Ex.: R4.5   (vem do script Python)");
  Serial.println("  U<val> (depuracao) fixa a umidade manualmente. Ex.: U45 | UX volta a ler o DHT22");
  Serial.println("  ?      mostra esta ajuda");
}

void processarComando(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  char tipo = toupper(cmd.charAt(0));
  if (tipo == 'R') {
    float v = cmd.substring(1).toFloat();
    chuvaPrevistaMm = (v < 0) ? 0 : v;
    Serial.printf(">> Chuva prevista atualizada: %.1f mm\n", chuvaPrevistaMm);
    ciclo(false);  // reavalia imediatamente com a nova previsão
  } else if (tipo == 'U') {
    if (cmd.length() > 1 && toupper(cmd.charAt(1)) == 'X') {
      umidadeManual = NAN;
      falhasDHT = 0;
      Serial.println(">> Umidade manual desativada: voltando a ler o DHT22.");
    } else {
      float v = cmd.substring(1).toFloat();
      umidadeManual = (v < 0) ? 0 : (v > 100 ? 100 : v);
      Serial.printf(">> Umidade manual (DEPURACAO): %.1f%%\n", umidadeManual);
    }
    ciclo(true);
  } else if (tipo == '?' || tipo == 'H') {
    mostrarAjuda();
  } else {
    Serial.println(">> Comando desconhecido. Digite ? para ajuda.");
  }
}

// Leitura não bloqueante do Monitor Serial (linhas terminadas em \n).
void lerSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (bufferSerial.length() > 0) {
        processarComando(bufferSerial);
        bufferSerial = "";
      }
    } else if (bufferSerial.length() < 32) {
      bufferSerial += c;
    }
  }
}

// ============================== SETUP/LOOP ===========================
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 3; i++) pinMode(botoes[i].pino, INPUT_PULLUP);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  acionarBomba(false);  // estado seguro na partida

  pinMode(PIN_DHT, INPUT_PULLUP);

  Serial.println("=== FarmTech Solutions | Fase 2 | Irrigacao inteligente (SOJA) ===");
  Serial.println("Botoes N/P/K: clique para alternar adequado/deficiente. LDR = pH. DHT22 = umidade do solo.");
  mostrarAjuda();
  Serial.println("CSV,tempo_s,umidade,ph,N,P,K,chuva_mm,bomba");

  delay(2000);          // o DHT22 precisa de ~2 s para a primeira leitura
  ciclo(true);
  ultimaLeitura = millis();
}

void loop() {
  bool mudouBotao = lerBotoes();
  lerSerial();

  unsigned long agora = millis();
  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;
    ciclo(true);                 // leitura periódica (inclui o DHT22)
  } else if (mudouBotao) {
    ciclo(false);                // reação imediata ao clique, sem reler o DHT22
  }

  delay(10);
}

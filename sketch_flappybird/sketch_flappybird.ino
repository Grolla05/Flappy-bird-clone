/*
 * Flappy Bird Clone
 * ------------------
 * Clone do clássico Flappy Bird na OLED 128x64. Um único botão aplica um
 * impulso vertical no pássaro; a gravidade o puxa continuamente para
 * baixo. Canos (obstáculos) com um vão (gap) são gerados
 * proceduralmente e se movem da direita para a esquerda. Colisão com um
 * cano ou com o teto/chão termina o jogo. O buzzer sinaliza pulo, ponto
 * e game over. A OLED exibe a pontuação atual e o recorde (salvo na
 * EEPROM, sobrevive a desligar o Arduino).
 *
 * Pinout:
 *   Botão PULAR -> D2
 *   Buzzer      -> D8
 *   OLED SDA    -> A4 (I2C)
 *   OLED SCL    -> A5 (I2C)
 *
 * Bibliotecas:
 *   Adafruit SSD1306
 *   Adafruit GFX
 *   EEPROM (nativa do Arduino)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ---------- Pinout ----------
#define PIN_BTN_PULAR 8
#define PIN_BUZZER    2

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDR     0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Layout (faixa amarela = placar, faixa azul = campo) ----------
const uint8_t HEADER_DIVIDER_Y = 15;
const uint8_t CAMPO_Y0         = 16;
const uint8_t CAMPO_Y1         = SCREEN_HEIGHT; // 64 (exclusivo)

// ---------- Física do pássaro ----------
const uint8_t PASSARO_X    = 24;   // posição horizontal fixa
const uint8_t PASSARO_RAIO = 3;    // desenhado como círculo preenchido
const float GRAVIDADE          = 0.35f;  // px/tick^2
const float IMPULSO_PULO       = -3.2f;  // px/tick aplicado ao pular
const float VELOCIDADE_MAX_QUEDA = 3.5f; // clamp de queda

float passaroY;    // posição vertical (centro do círculo)
float passaroVelY; // velocidade vertical

// ---------- Canos ----------
const uint8_t  CANO_LARGURA      = 10; // px
const uint8_t  GAP_ALTURA        = 20; // px de abertura entre cano de cima e de baixo
const uint8_t  GAP_MARGEM        = 4;  // distância mínima do gap até teto/chão do campo
const int8_t   CANO_VELOCIDADE   = 2;  // px/tick
const uint8_t  MAX_CANOS         = 3;
const int16_t  ESPACAMENTO_CANOS = 44; // distância horizontal entre canos consecutivos

struct Cano {
  int16_t x;
  uint8_t gapY;    // topo do vão (gap vai de gapY a gapY+GAP_ALTURA)
  bool pontuado;
};
Cano canos[MAX_CANOS];

// ---------- Placar ----------
uint16_t pontuacao = 0;
uint16_t recorde = 0;
const int ENDERECO_EEPROM_RECORDE = 0;

// ---------- Estado do jogo ----------
enum Estado : uint8_t { TELA_INICIAL, JOGANDO, GAME_OVER };
Estado estado = TELA_INICIAL;

// ---------- Timing não-bloqueante ----------
const unsigned long FRAME_MS = 40; // ~25 fps de física/desenho
unsigned long proximoFrame = 0;

// ---------- Debounce do botão (borda de descida = pressionado) ----------
const unsigned long DEBOUNCE_MS = 40;
bool estadoEstavel   = HIGH;
bool ultimaLeitura    = HIGH;
unsigned long ultimaMudanca = 0;
bool bordaPendente    = false; // true = houve um "pressionar" ainda não consumido

void setup() {
  pinMode(PIN_BTN_PULAR, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // Trava aqui se a OLED não inicializar — sinal de erro de fiação/endereço I2C
    for (;;) {}
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  randomSeed(analogRead(A0)); // pino analógico flutuante (não usado) garante uma seed variável

  EEPROM.get(ENDERECO_EEPROM_RECORDE, recorde);
  if (recorde == 0xFFFF) recorde = 0; // EEPROM "virgem" lê 0xFF em todos os bytes

  desenharTelaInicial();
}

void loop() {
  lerBotao();

  switch (estado) {
    case TELA_INICIAL:
      if (consumirBordaBotao()) {
        iniciarJogo();
      }
      break;

    case JOGANDO: {
      if (consumirBordaBotao()) {
        passaroVelY = IMPULSO_PULO;
        tocarBeep(1800, 30); // chirp agudo do pulo
      }

      unsigned long agora = millis();
      if (agora >= proximoFrame) {
        proximoFrame = agora + FRAME_MS;
        atualizarFisica();
      }
      break;
    }

    case GAME_OVER:
      if (consumirBordaBotao()) {
        desenharTelaInicial();
        estado = TELA_INICIAL;
      }
      break;
  }
}

// =====================================================================
// Botão
// =====================================================================
void lerBotao() {
  bool leitura = digitalRead(PIN_BTN_PULAR);
  if (leitura != ultimaLeitura) {
    ultimaMudanca = millis();
  }
  if ((millis() - ultimaMudanca) > DEBOUNCE_MS) {
    if (leitura != estadoEstavel) {
      estadoEstavel = leitura;
      if (estadoEstavel == LOW) { // borda de descida = pressionado
        bordaPendente = true;
      }
    }
  }
  ultimaLeitura = leitura;
}

// Consome a borda de "pressionar" pendente (uma vez só por aperto).
bool consumirBordaBotao() {
  if (bordaPendente) {
    bordaPendente = false;
    return true;
  }
  return false;
}

// =====================================================================
// Ciclo de vida do jogo
// =====================================================================
void iniciarJogo() {
  passaroY = (CAMPO_Y0 + CAMPO_Y1) / 2.0f;
  passaroVelY = 0;
  pontuacao = 0;

  for (uint8_t i = 0; i < MAX_CANOS; i++) {
    canos[i].x = SCREEN_WIDTH + i * ESPACAMENTO_CANOS;
    canos[i].gapY = sortearGapY();
    canos[i].pontuado = false;
  }

  proximoFrame = millis() + FRAME_MS;
  estado = JOGANDO;
  desenharTela();
}

uint8_t sortearGapY() {
  return random(CAMPO_Y0 + GAP_MARGEM, CAMPO_Y1 - GAP_ALTURA - GAP_MARGEM + 1);
}

void atualizarFisica() {
  // Gravidade
  passaroVelY += GRAVIDADE;
  if (passaroVelY > VELOCIDADE_MAX_QUEDA) passaroVelY = VELOCIDADE_MAX_QUEDA;
  passaroY += passaroVelY;

  // Colisão com teto/chão do campo
  if (passaroY - PASSARO_RAIO <= CAMPO_Y0 || passaroY + PASSARO_RAIO >= CAMPO_Y1) {
    finalizarJogo();
    return;
  }

  // Move os canos e verifica pontuação/colisão
  for (uint8_t i = 0; i < MAX_CANOS; i++) {
    canos[i].x -= CANO_VELOCIDADE;

    // Cano saiu totalmente da tela pela esquerda: reaparece à direita do último
    if (canos[i].x + CANO_LARGURA < 0) {
      int16_t maiorX = canos[0].x;
      for (uint8_t j = 1; j < MAX_CANOS; j++) {
        if (canos[j].x > maiorX) maiorX = canos[j].x;
      }
      canos[i].x = maiorX + ESPACAMENTO_CANOS;
      canos[i].gapY = sortearGapY();
      canos[i].pontuado = false;
      continue;
    }

    // Pontuação: cano ultrapassou totalmente o pássaro
    if (!canos[i].pontuado && (canos[i].x + CANO_LARGURA) < (PASSARO_X - PASSARO_RAIO)) {
      canos[i].pontuado = true;
      pontuacao++;
      tocarBeep(2600, 25); // beep curto de ponto
    }

    // Colisão: cano se sobrepõe horizontalmente ao pássaro?
    bool sobrepoeX = (canos[i].x < PASSARO_X + PASSARO_RAIO) &&
                      (canos[i].x + CANO_LARGURA > PASSARO_X - PASSARO_RAIO);
    if (sobrepoeX) {
      bool dentroDoGap = (passaroY - PASSARO_RAIO >= canos[i].gapY) &&
                          (passaroY + PASSARO_RAIO <= canos[i].gapY + GAP_ALTURA);
      if (!dentroDoGap) {
        finalizarJogo();
        return;
      }
    }
  }

  desenharTela();
}

void finalizarJogo() {
  tocarBeep(150, 300); // beep grave de game over

  bool novoRecorde = false;
  if (pontuacao > recorde) {
    recorde = pontuacao;
    EEPROM.put(ENDERECO_EEPROM_RECORDE, recorde);
    novoRecorde = true;
  }

  estado = GAME_OVER;
  desenharTelaGameOver(novoRecorde);
}

// =====================================================================
// Som
// =====================================================================
void tocarBeep(unsigned int frequencia, unsigned int duracaoMs) {
  tone(PIN_BUZZER, frequencia, duracaoMs); // tone() com duração não bloqueia o loop
}

// =====================================================================
// Desenho
// =====================================================================
void desenharPlacar() {
  display.setTextSize(1);

  char textoPontos[10];
  char textoRecorde[10];
  sprintf(textoPontos, "SCORE:%u", pontuacao);
  sprintf(textoRecorde, "BEST:%u", recorde);

  display.setCursor(4, 4);
  display.print(textoPontos);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(textoRecorde, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 4, 4);
  display.print(textoRecorde);
}

void desenharTela() {
  display.clearDisplay();

  desenharPlacar();
  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  // Canos (retângulo de cima + retângulo de baixo, com o gap entre eles)
  for (uint8_t i = 0; i < MAX_CANOS; i++) {
    if (canos[i].x + CANO_LARGURA < 0 || canos[i].x >= SCREEN_WIDTH) continue;

    uint8_t alturaTopo = canos[i].gapY - CAMPO_Y0;
    display.fillRect(canos[i].x, CAMPO_Y0, CANO_LARGURA, alturaTopo, SSD1306_WHITE);

    uint8_t inicioBaixo = canos[i].gapY + GAP_ALTURA;
    uint8_t alturaBaixo = CAMPO_Y1 - inicioBaixo;
    display.fillRect(canos[i].x, inicioBaixo, CANO_LARGURA, alturaBaixo, SSD1306_WHITE);
  }

  // Pássaro
  display.fillCircle(PASSARO_X, (int16_t)passaroY, PASSARO_RAIO, SSD1306_WHITE);

  display.display();
}

void desenharTelaInicial() {
  display.clearDisplay();

  display.setTextSize(2);
  const char *titulo = "FLAPPY";
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(titulo, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 0);
  display.print(titulo);

  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  display.setTextSize(1);
  const char *msg = "Aperte o botao";
  display.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 34);
  display.print(msg);

  char textoRecorde[16];
  sprintf(textoRecorde, "Recorde: %u", recorde);
  display.getTextBounds(textoRecorde, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 48);
  display.print(textoRecorde);

  display.display();
}

void desenharTelaGameOver(bool novoRecorde) {
  display.clearDisplay();

  display.setTextSize(1);
  const char *titulo = "GAME OVER";
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(titulo, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 4);
  display.print(titulo);

  display.drawFastHLine(0, HEADER_DIVIDER_Y, SCREEN_WIDTH, SSD1306_WHITE);

  char textoPontos[16];
  sprintf(textoPontos, "Pontos: %u", pontuacao);
  display.getTextBounds(textoPontos, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 28);
  display.print(textoPontos);

  const char *linha2 = novoRecorde ? "NOVO RECORDE!" : "Aperte o botao";
  display.getTextBounds(linha2, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 44);
  display.print(linha2);

  display.display();
}

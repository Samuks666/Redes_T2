/*
 * Trabalho Prático - Rede de Sensores Sem Fio
 * Arquivo: sensor_2.ino -> PLACA SENSOR 2 (S2)
 * Compatível com: ESP32-C3, ESP32 clássico e ESP8266 na Arduino IDE
 *
 * Funcionalidades:
 * - Ordena os roteadores encontrados por potência de sinal (RSSI)
 * - Antes de aceitar um roteador como pai, testa com "CHECK_COLETOR" se ele
 *   REALMENTE fala com o Coletor (ROTA_OK) ou se é um caminho sem saída (ROTA_MORTA)!
 * - Se um roteador estiver sem saída (ex: R2 isolado), o sensor rejeita esse roteador
 *   mesmo que o RSSI dele seja forte e conecta automaticamente no outro roteador!
 */

#if defined(ESP32)
  #include <WiFi.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
#endif
#include <WiFiUdp.h>

// ===================== CONFIGURAÇÃO DO SENSOR 2 =====================
#define SENSOR_ID "S2"

const char* PREFIXO_ROTEADOR = "GRUPO_R"; // Aceita GRUPO_R1, GRUPO_R2, GRUPO_R3, etc.
const char* WIFI_PASS        = "senha1234";
const uint16_t UDP_PORT      = 5000;

const unsigned long INTERVALO_ENVIO_MS = 1000; // 1 mensagem por segundo
const unsigned long TIMEOUT_ACK_MS     = 600;  // Tempo máximo esperando ACK/ROTA_OK
const int MAX_FALHAS_SEGUIDAS          = 3;    // Troca de roteador após 3 falhas
// ====================================================================

struct CandidatoRoteador {
  String ssid;
  int rssi;
  int canal;
};

WiFiUDP udp;
uint32_t seqNum = 0;
long ultimoRttMs = -1;
String ssidPaiAtual = "";
int falhasSeguidas = 0;
unsigned long ultimoEnvioMs = 0;

// Pergunta ao roteador recém-conectado se ele tem caminho ativo até o Coletor
bool roteadorFalaComColetor() {
  IPAddress ipRoteador = WiFi.gatewayIP();
  if (ipRoteador == IPAddress(0, 0, 0, 0)) {
    return false;
  }

  while (udp.parsePacket() > 0) {
    udp.flush();
  }

  char pergunta[48];
  snprintf(pergunta, sizeof(pergunta), "CHECK_COLETOR;%s", SENSOR_ID);

  for (int tentativa = 0; tentativa < 2; tentativa++) {
    udp.beginPacket(ipRoteador, UDP_PORT);
    udp.write((const uint8_t*)pergunta, strlen(pergunta));
    udp.endPacket();

    unsigned long inicio = millis();
    char buf[64];
    while (millis() - inicio < TIMEOUT_ACK_MS) {
      int tam = udp.parsePacket();
      if (tam > 0) {
        int lidos = udp.read(buf, sizeof(buf) - 1);
        buf[lidos] = '\0';

        if (strncmp(buf, "ROTA_OK", 7) == 0 || strncmp(buf, "PONG;OK", 7) == 0) {
          return true;
        }
        if (strncmp(buf, "ROTA_MORTA", 10) == 0) {
          return false;
        }
      }
      delay(2);
    }
    delay(40);
  }
  return false;
}

bool conectarETestarRoteador(const String& ssidAlvo, int rssiScan) {
  Serial.printf("[%s] Conectando em %s (%d dBm) para testar caminho ate o Coletor...\n",
                SENSOR_ID, ssidAlvo.c_str(), rssiScan);
  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssidAlvo.c_str(), WIFI_PASS);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - inicio) < 5500) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%s] Falha ao associar no Wi-Fi %s.\n", SENSOR_ID, ssidAlvo.c_str());
    return false;
  }

  delay(200);

  // Agora testa se esse roteador realmente chega no Coletor ou se dá em nada!
  if (roteadorFalaComColetor()) {
    ssidPaiAtual = ssidAlvo;
    falhasSeguidas = 0;
    Serial.printf("[%s] APROVADO! %s fala com o Coletor! (Meu IP=%s | Gateway=%s | RSSI=%ld dBm)\n",
                  SENSOR_ID,
                  ssidPaiAtual.c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
  }

  Serial.printf("[%s] REJEITADO: %s tem sinal Wi-Fi, mas NAO fala com o Coletor (caminho sem saida)!\n",
                SENSOR_ID, ssidAlvo.c_str());
  WiFi.disconnect(false);
  return false;
}

// Escaneia todos os roteadores GRUPO_R*, ordena por potência (RSSI) e testa até achar um com rota válida!
void escolherEMudarRoteador() {
  Serial.printf("\n[%s] Escaneando roteadores disponiveis (GRUPO_R*)...\n", SENSOR_ID);
  int n = WiFi.scanNetworks();

  CandidatoRoteador candidatos[10];
  int totalCandidatos = 0;

  for (int i = 0; i < n && totalCandidatos < 10; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.startsWith(PREFIXO_ROTEADOR)) {
      candidatos[totalCandidatos].ssid  = ssid;
      candidatos[totalCandidatos].rssi  = WiFi.RSSI(i);
      candidatos[totalCandidatos].canal = WiFi.channel(i);
      Serial.printf("  [SCAN] Encontrado: %s (%d dBm, canal %d)\n",
                    ssid.c_str(), candidatos[totalCandidatos].rssi, candidatos[totalCandidatos].canal);
      totalCandidatos++;
    }
  }
  WiFi.scanDelete();

  if (totalCandidatos == 0) {
    Serial.printf("[%s] Nenhum roteador GRUPO_R* encontrado no ar no momento.\n", SENSOR_ID);
    return;
  }

  // Ordena do maior RSSI (sinal mais forte) para o menor
  for (int i = 0; i < totalCandidatos - 1; i++) {
    for (int j = i + 1; j < totalCandidatos; j++) {
      if (candidatos[j].rssi > candidatos[i].rssi) {
        CandidatoRoteador temp = candidatos[i];
        candidatos[i] = candidatos[j];
        candidatos[j] = temp;
      }
    }
  }

  // Testa cada roteador em ordem de potência: só aceita aquele que realmente fala com o Coletor!
  for (int i = 0; i < totalCandidatos; i++) {
    if (conectarETestarRoteador(candidatos[i].ssid, candidatos[i].rssi)) {
      return; // Achou um roteador com caminho válido até o Coletor!
    }
  }

  Serial.printf("[%s] Nenhum dos roteadores encontrados tem caminho ate o Coletor no momento!\n", SENSOR_ID);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== INICIANDO SENSOR " SENSOR_ID " ===");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
#if defined(ESP32)
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);
#endif

  udp.begin(UDP_PORT);
  escolherEMudarRoteador();
}

void loop() {
  unsigned long agora = millis();
  if (agora - ultimoEnvioMs < INTERVALO_ENVIO_MS) {
    return;
  }
  ultimoEnvioMs = agora;

  seqNum++;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[%s][SEQ %lu] Sem associacao Wi-Fi. Buscando roteador com rota...\n",
                  SENSOR_ID, (unsigned long)seqNum);
    ultimoRttMs = -1;
    escolherEMudarRoteador();
    return;
  }

  long rssiPai = WiFi.RSSI();
  IPAddress ipPai = WiFi.gatewayIP();

  char mensagem[160];
  snprintf(mensagem, sizeof(mensagem), "DATA;%s;%lu;%ld;%s;%ld",
           SENSOR_ID, (unsigned long)seqNum, ultimoRttMs, SENSOR_ID, rssiPai);

  while (udp.parsePacket() > 0) {
    udp.flush();
  }

  unsigned long tEnvio = millis();
  udp.beginPacket(ipPai, UDP_PORT);
  udp.write((const uint8_t*)mensagem, strlen(mensagem));
  udp.endPacket();

  Serial.printf("[%s][ENVIO] %s -> %s (%s)\n",
                SENSOR_ID, mensagem, ssidPaiAtual.c_str(), ipPai.toString().c_str());

  bool recebeuAck = false;
  bool rotaMortaDetectada = false;
  char bufferAck[64];
  char ackEsperado[32];
  snprintf(ackEsperado, sizeof(ackEsperado), "ACK;%s;%lu", SENSOR_ID, (unsigned long)seqNum);

  while (millis() - tEnvio < TIMEOUT_ACK_MS) {
    int tam = udp.parsePacket();
    if (tam > 0) {
      int lidos = udp.read(bufferAck, sizeof(bufferAck) - 1);
      bufferAck[lidos] = '\0';

      if (strncmp(bufferAck, ackEsperado, strlen(ackEsperado)) == 0) {
        ultimoRttMs = millis() - tEnvio;
        recebeuAck = true;
        falhasSeguidas = 0;
        Serial.printf("[%s][ACK] Recebido em %ld ms: %s\n", SENSOR_ID, ultimoRttMs, bufferAck);
        break;
      }
      // Se o roteador avisou que ficou sem saída (ROTA_MORTA), já troca na hora!
      if (strncmp(bufferAck, "ROTA_MORTA", 10) == 0) {
        rotaMortaDetectada = true;
        break;
      }
    }
    delay(2);
  }

  if (rotaMortaDetectada) {
    ultimoRttMs = -1;
    Serial.printf("[%s][ROTA MORTA] %s informou que perdeu caminho ate o Coletor! Trocando agora...\n",
                  SENSOR_ID, ssidPaiAtual.c_str());
    escolherEMudarRoteador();
  } else if (!recebeuAck) {
    ultimoRttMs = -1;
    falhasSeguidas++;
    Serial.printf("[%s][TIMEOUT] Sem ACK de %s (%d/%d)\n",
                  SENSOR_ID, ssidPaiAtual.c_str(), falhasSeguidas, MAX_FALHAS_SEGUIDAS);

    if (falhasSeguidas >= MAX_FALHAS_SEGUIDAS) {
      Serial.printf("[%s][RECUPERACAO] Limite de falhas atingido! Buscando roteador com caminho ativo...\n",
                    SENSOR_ID);
      escolherEMudarRoteador();
    }
  }
}

/*
 * Trabalho Prático - Rede de Sensores Sem Fio
 * Arquivo: roteador_1.ino -> PLACA ROTEADOR 1 (R1)
 * Compatível com: ESP32-C3 e ESP32 clássico na Arduino IDE
 *
 * Funcionalidades:
 * - Testa se o pai candidato REALMENTE fala com o Coletor (CHECK_COLETOR -> ROTA_OK / ROTA_MORTA)
 * - Se este roteador ficar isolado (sem enxergar Coletor nem outro roteador com rota),
 *   avisa aos sensores que é um "caminho sem saída" (ROTA_MORTA), fazendo o sensor
 *   trocar para outro roteador mesmo que o sinal (RSSI) deste seja mais forte!
 * - Comandos no Monitor Serial:
 *     'f' -> Simula falha apenas do caminho direto até o Coletor (força ir pelo outro roteador)
 *     'i' -> Simula isolamento total deste roteador ("não enxerga mais nada" / caminho sem saída)
 *     's' -> Escaneia redes Wi-Fi no ar
 */

#include <WiFi.h>
#include <WiFiUdp.h>

// ===================== CONFIGURAÇÃO DO ROTEADOR 1 =====================
#define ROUTER_NUM 1

// true  = Teste sem PC (R1 funciona como Roteador + Coletor final no Monitor Serial)
// false = Modo oficial com PC (R1 e R2 enviam para o Hotspot GRUPO_COLETOR do Linux)
#define MODO_TESTE_SEM_PC true

const int CANAL_WIFI             = 6;
const char* WIFI_PASS            = "senha1234";
const char* PREFIXO_ROTEADOR     = "GRUPO_R";
const char* SSID_COLETOR         = "GRUPO_COLETOR";
const uint16_t UDP_PORT          = 5000;

const unsigned long TIMEOUT_PAI_MS               = 500;   // Timeout esperando ACK/ROTA_OK do pai
const unsigned long INTERVALO_PING_MS            = 2000;  // Heartbeat quando conectado ao pai
const unsigned long INTERVALO_BUSCA_MS           = 8000;  // Intervalo entre buscas se estiver sem pai
const unsigned long INTERVALO_VOLTAR_COLETOR_MS  = 15000; // Se estiver em outro roteador, testa a cada 15s se o Coletor direto voltou
const int MAX_FALHAS_PAI                         = 3;     // Falhas até trocar de pai
// ======================================================================

char MEU_ID[12];
char MEU_SSID[32];
const IPAddress MEU_AP_IP(10, 0, ROUTER_NUM, 1);
const IPAddress SUBNET_MASK(255, 255, 255, 0);

enum TipoPai {
  PAI_NENHUM = 0,
  PAI_COLETOR = 1,
  PAI_OUTRO_ROTEADOR = 2,
  PAI_COLETOR_LOCAL_TESTE = 3
};

WiFiUDP udp;
TipoPai paiAtual = PAI_NENHUM;
String ssidPaiAtual = "NENHUM";
String rotaAteColetor = "";   // Ex: "R1->C" ou "R1->R2->C" ou "R1->R2->R3->C"
int saltosAteColetor = 999;   // 1 = direto no Coletor, 2 = via 1 roteador, 3 = via 2 roteadores...
int falhasPai = 0;
bool bloquearColetor = false; // Comando 'f': bloqueia só o caminho direto para o Coletor
bool isolarRoteador  = false; // Comando 'i': roteador não enxerga mais ninguém (caminho sem saída)
unsigned long ultimoPingMs = 0;
unsigned long ultimaBuscaMs = 0;
unsigned long ultimoTesteColetorDiretoMs = 0;

void iniciarMeuAP() {
  WiFi.softAPConfig(MEU_AP_IP, MEU_AP_IP, SUBNET_MASK);
  delay(100);
  WiFi.softAP(MEU_SSID, WIFI_PASS, CANAL_WIFI, 0, 4);
  delay(100);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setSleep(false);

  Serial.printf("[%s] AP iniciado: SSID=%s | IP=%s | Canal=%d\n",
                MEU_ID, MEU_SSID, WiFi.softAPIP().toString().c_str(), CANAL_WIFI);
}

// Pergunta ao pai conectado se ele tem caminho ativo até o Coletor e descobre a rota completa + nº de saltos
bool testarCaminhoAteColetorNoPai(TipoPai tipoPretendido) {
  if (isolarRoteador) {
    return false;
  }
  if (bloquearColetor && tipoPretendido == PAI_COLETOR) {
    return false;
  }
  if (tipoPretendido == PAI_COLETOR_LOCAL_TESTE) {
    saltosAteColetor = 1;
    rotaAteColetor = String(MEU_ID) + "->C";
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  IPAddress ipAlvo = WiFi.gatewayIP();
  if (ipAlvo == IPAddress(0, 0, 0, 0)) {
    return false;
  }

  char pergunta[48];
  snprintf(pergunta, sizeof(pergunta), "CHECK_COLETOR;%s", MEU_ID);

  for (int tentativa = 0; tentativa < 2; tentativa++) {
    udp.beginPacket(ipAlvo, UDP_PORT);
    udp.write((const uint8_t*)pergunta, strlen(pergunta));
    udp.endPacket();

    unsigned long inicio = millis();
    char buf[96];
    while (millis() - inicio < TIMEOUT_PAI_MS) {
      int tam = udp.parsePacket();
      if (tam > 0) {
        IPAddress remIP = udp.remoteIP();
        int lidos = udp.read(buf, sizeof(buf) - 1);
        buf[lidos] = '\0';

        if (bloquearColetor && tipoPretendido == PAI_COLETOR && remIP == ipAlvo) {
          continue;
        }

        if (remIP == ipAlvo) {
          // Resposta do Coletor ("ROTA_OK;C") ou de outro Roteador ("ROTA_OK;R2->R3->C")
          if (strncmp(buf, "ROTA_OK", 7) == 0 || strncmp(buf, "PONG;OK", 7) == 0) {
            char* rotaDoPai = strchr(buf, ';');
            if (rotaDoPai) {
              rotaDoPai++; // pula ';'
              // Prevenção de laço para qualquer número de roteadores (R1->R2->R3->R1):
              // se meu próprio ID já está na rota do pai até o Coletor, rejeita!
              String strRotaPai(rotaDoPai);
              if (strRotaPai.indexOf(MEU_ID) >= 0) {
                return false;
              }
              rotaAteColetor = String(MEU_ID) + "->" + strRotaPai;
              // Conta quantos saltos ("->") existem na rota até o Coletor
              int saltos = 0;
              for (unsigned int k = 0; k + 1 < rotaAteColetor.length(); k++) {
                if (rotaAteColetor[k] == '-' && rotaAteColetor[k + 1] == '>') saltos++;
              }
              saltosAteColetor = (saltos > 0) ? saltos : 1;
            } else {
              saltosAteColetor = (tipoPretendido == PAI_COLETOR) ? 1 : 2;
              rotaAteColetor = String(MEU_ID) + "->C";
            }
            return true;
          }
          if (strncmp(buf, "ROTA_MORTA", 10) == 0) {
            return false;
          }
        }
      }
      delay(2);
    }
    delay(40);
  }
  return false;
}

bool tentarAssociar(const String& ssidAlvo, TipoPai tipo) {
  if (isolarRoteador) {
    return false;
  }

  Serial.printf("[%s] Conectando em %s para testar se chega ao Coletor...\n",
                MEU_ID, ssidAlvo.c_str());

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
    Serial.printf("[%s] Nao conseguiu associar ao Wi-Fi %s.\n", MEU_ID, ssidAlvo.c_str());
    return false;
  }

  delay(250);

  if (testarCaminhoAteColetorNoPai(tipo)) {
    paiAtual = tipo;
    ssidPaiAtual = ssidAlvo;
    falhasPai = 0;
    Serial.printf("[%s] ROTA CONFIRMADA ate o Coletor via %s! (Gateway=%s, RSSI=%ld dBm)\n",
                  MEU_ID,
                  ssidPaiAtual.c_str(),
                  WiFi.gatewayIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
  }

  Serial.printf("[%s] REJEITADO: %s tem sinal Wi-Fi, mas NAO chega ao Coletor (caminho sem saida/laco)!\n",
                MEU_ID, ssidAlvo.c_str());
  WiFi.disconnect(false);
  return false;
}

void escolherPai() {
  ultimaBuscaMs = millis();

  if (isolarRoteador) {
    paiAtual = PAI_NENHUM;
    ssidPaiAtual = "ISOLADO (SEM ROTA)";
    WiFi.disconnect(false);
    Serial.printf("[%s] Roteador em modo ISOLADO: nao enxerga Coletor nem outros roteadores.\n", MEU_ID);
    return;
  }

#if MODO_TESTE_SEM_PC && (ROUTER_NUM == 1)
  if (!bloquearColetor) {
    paiAtual = PAI_COLETOR_LOCAL_TESTE;
    ssidPaiAtual = "COLETOR_LOCAL_R1";
    falhasPai = 0;
    Serial.printf("[%s] Modo Teste Sem PC ativo: %s operando como Coletor Final.\n", MEU_ID, MEU_ID);
    return;
  }
#endif

  paiAtual = PAI_NENHUM;
  ssidPaiAtual = "NENHUM";

  Serial.printf("[%s] Escaneando redes em busca de caminho ate o Coletor...\n", MEU_ID);
  int n = WiFi.scanNetworks();

  bool achouColetor = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == String(SSID_COLETOR)) {
      achouColetor = true;
      break;
    }
  }

  // 1ª opção: Caminho direto ao Coletor C
  if (achouColetor && !bloquearColetor) {
    WiFi.scanDelete();
    if (tentarAssociar(SSID_COLETOR, PAI_COLETOR)) {
      return;
    }
    n = WiFi.scanNetworks();
  }

  // 2ª opção: Outro roteador GRUPO_R* (testando se ele realmente chega ao Coletor!)
  for (int i = 0; i < n; i++) {
    String ssidEncontrado = WiFi.SSID(i);
    if (ssidEncontrado.startsWith(PREFIXO_ROTEADOR) && ssidEncontrado != String(MEU_SSID)) {
      Serial.printf("  [SCAN] Testando candidato %s (%ld dBm)...\n",
                    ssidEncontrado.c_str(), WiFi.RSSI(i));
      if (tentarAssociar(ssidEncontrado, PAI_OUTRO_ROTEADOR)) {
        WiFi.scanDelete();
        return;
      }
    }
  }
  WiFi.scanDelete();

  Serial.printf("[%s] AVISO: Nenhum caminho ate o Coletor disponivel! (%s esta sem saida).\n",
                MEU_ID, MEU_ID);
}

void escanearCanal() {
  Serial.println("\n--- SCAN DE REDES WI-FI VIZINHAS ---");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.printf("  %2d: SSID=%-24s | Canal=%2d | RSSI=%4ld dBm\n",
                  i + 1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  }
  WiFi.scanDelete();
  Serial.println("------------------------------------\n");
}

void lerComandosSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'f' || c == 'F') {
      bloquearColetor = !bloquearColetor;
      Serial.printf("\n[COMANDO SERIAL] Bloqueio do caminho direto ao Coletor: %s\n",
                    bloquearColetor ? "ATIVADO (roteador vai perceber pelos timeouts)" : "DESATIVADO");
      if (!bloquearColetor && paiAtual != PAI_COLETOR) {
        escolherPai();
      }
    } else if (c == 'i' || c == 'I') {
      isolarRoteador = !isolarRoteador;
      Serial.printf("\n[COMANDO SERIAL] Isolamento total de %s (caminho que da em nada): %s\n",
                    MEU_ID, isolarRoteador ? "ATIVADO (vai perder rota pelos timeouts)" : "DESATIVADO");
      if (!isolarRoteador) {
        escolherPai();
      }
    } else if (c == 's' || c == 'S') {
      escanearCanal();
    }
  }
}

// Verifica na hora se este roteador consegue chegar ao Coletor (sem formar laço com quem perguntou)
bool tenhoRotaVivaParaColetor(const char* idQuemPerguntou) {
  if (isolarRoteador || paiAtual == PAI_NENHUM) {
    return false;
  }

  // Se quem está perguntando já faz parte da minha rota até o Coletor (ex: R1->R2->R3), seria laço!
  if (idQuemPerguntou && strlen(idQuemPerguntou) > 0) {
    String ssidDeQuemPerguntou = String("GRUPO_") + idQuemPerguntou;
    if (ssidPaiAtual == ssidDeQuemPerguntou || rotaAteColetor.indexOf(idQuemPerguntou) >= 0) {
      return false;
    }
  }

  if (paiAtual == PAI_COLETOR_LOCAL_TESTE) {
    rotaAteColetor = String(MEU_ID) + "->C";
    saltosAteColetor = 1;
    return !bloquearColetor;
  }

  // Testa ativamente o meu pai agora para garantir que o caminho não caiu
  return testarCaminhoAteColetorNoPai(paiAtual);
}

void encaminharMensagem(char* msgRecebida, IPAddress ipFilho, uint16_t portaFilho) {
  if (isolarRoteador || paiAtual == PAI_NENHUM) {
    // Avisa imediatamente ao filho que este roteador está sem saída para o Coletor!
    udp.beginPacket(ipFilho, portaFilho);
    udp.print("ROTA_MORTA");
    udp.endPacket();
    Serial.printf("[%s] Pacote recusado: estou SEM ROTA para o Coletor! Avisando filho.\n", MEU_ID);
    return;
  }

  String strMsg(msgRecebida);
  if (strMsg.indexOf(String("->") + MEU_ID) >= 0) {
    Serial.printf("[%s] LACO DETECTADO no pacote (%s). Descartando!\n", MEU_ID, msgRecebida);
    return;
  }

  char copia[180];
  strncpy(copia, msgRecebida, sizeof(copia) - 1);
  copia[sizeof(copia) - 1] = '\0';

  char* tipo      = strtok(copia, ";");
  char* origem    = strtok(NULL, ";");
  char* seq       = strtok(NULL, ";");
  char* rttAnt    = strtok(NULL, ";");
  char* caminho   = strtok(NULL, ";");
  char* potencias = strtok(NULL, ";");

  if (!tipo || !origem || !seq || !rttAnt || !caminho || !potencias) {
    return;
  }

  if (paiAtual == PAI_COLETOR_LOCAL_TESTE) {
    char caminhoFinal[80];
    snprintf(caminhoFinal, sizeof(caminhoFinal), "%s->%s", caminho, MEU_ID);
    Serial.printf("[COLETOR-TESTE EM %s] CSV -> %lu,%s,%s,%s,%s,%s\n",
                  MEU_ID, millis(), origem, seq, rttAnt, caminhoFinal, potencias);

    char ackMsg[48];
    snprintf(ackMsg, sizeof(ackMsg), "ACK;%s;%s", origem, seq);
    udp.beginPacket(ipFilho, portaFilho);
    udp.write((const uint8_t*)ackMsg, strlen(ackMsg));
    udp.endPacket();
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    paiAtual = PAI_NENHUM;
    udp.beginPacket(ipFilho, portaFilho);
    udp.print("ROTA_MORTA");
    udp.endPacket();
    return;
  }

  long rssiDoMeuPai = WiFi.RSSI();
  IPAddress destPai = WiFi.gatewayIP();

  char msgEncaminhada[200];
  snprintf(msgEncaminhada, sizeof(msgEncaminhada), "DATA;%s;%s;%s;%s->%s;%s,%ld",
           origem, seq, rttAnt, caminho, MEU_ID, potencias, rssiDoMeuPai);

  if (!(bloquearColetor && paiAtual == PAI_COLETOR)) {
    udp.beginPacket(destPai, UDP_PORT);
    udp.write((const uint8_t*)msgEncaminhada, strlen(msgEncaminhada));
    udp.endPacket();
  }

  Serial.printf("[%s] Encaminhando para %s (%s): %s\n",
                MEU_ID, ssidPaiAtual.c_str(), destPai.toString().c_str(), msgEncaminhada);

  char ackEsperado[32];
  snprintf(ackEsperado, sizeof(ackEsperado), "ACK;%s;%s", origem, seq);

  unsigned long inicio = millis();
  char bufAck[64];
  bool recebeuAckDoPai = false;

  while (millis() - inicio < TIMEOUT_PAI_MS) {
    int tam = udp.parsePacket();
    if (tam > 0) {
      IPAddress remIP = udp.remoteIP();
      int lidos = udp.read(bufAck, sizeof(bufAck) - 1);
      bufAck[lidos] = '\0';

      if (bloquearColetor && paiAtual == PAI_COLETOR && remIP == destPai) {
        continue;
      }

      if (strncmp(bufAck, ackEsperado, strlen(ackEsperado)) == 0) {
        recebeuAckDoPai = true;
        falhasPai = 0;

        udp.beginPacket(ipFilho, portaFilho);
        udp.write((const uint8_t*)bufAck, strlen(bufAck));
        udp.endPacket();
        break;
      }
    }
    delay(1);
  }

  if (!recebeuAckDoPai) {
    falhasPai++;
    Serial.printf("[%s] Timeout esperando ACK do pai %s (%d/%d)\n",
                  MEU_ID, ssidPaiAtual.c_str(), falhasPai, MAX_FALHAS_PAI);
    if (falhasPai >= MAX_FALHAS_PAI) {
      Serial.printf("[%s] Queda do pai detectada! Buscando novo caminho...\n", MEU_ID);
      escolherPai();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  snprintf(MEU_ID, sizeof(MEU_ID), "R%d", ROUTER_NUM);
  snprintf(MEU_SSID, sizeof(MEU_SSID), "%s%d", PREFIXO_ROTEADOR, ROUTER_NUM);

  Serial.printf("\n=== INICIANDO ROTEADOR %s ===\n", MEU_ID);
  Serial.println("Comandos seriais: 'f' = cortar/voltar Coletor direto | 'i' = isolar roteador (sem saida) | 's' = scan");

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  iniciarMeuAP();

  udp.begin(UDP_PORT);
  escolherPai();
}

void loop() {
  lerComandosSerial();

  int tam = udp.parsePacket();
  if (tam > 0) {
    IPAddress ipRemoto = udp.remoteIP();
    uint16_t portaRemota = udp.remotePort();
    char buffer[200];
    int lidos = udp.read(buffer, sizeof(buffer) - 1);
    buffer[lidos] = '\0';

    // Quando um Sensor ou outro Roteador pergunta se este roteador chega até o Coletor:
    if (strncmp(buffer, "CHECK_COLETOR", 13) == 0 || strncmp(buffer, "PING", 4) == 0) {
      char* idOrigem = strchr(buffer, ';');
      if (idOrigem) idOrigem++; // pula o ';'

      bool rotaOk = tenhoRotaVivaParaColetor(idOrigem);

      udp.beginPacket(ipRemoto, portaRemota);
      if (rotaOk) {
        udp.printf("ROTA_OK;%s", rotaAteColetor.c_str());
      } else {
        udp.print("ROTA_MORTA");
      }
      udp.endPacket();
    } else if (strncmp(buffer, "DATA;", 5) == 0) {
      encaminharMensagem(buffer, ipRemoto, portaRemota);
    }
  }

  unsigned long agora = millis();
  if (isolarRoteador || paiAtual == PAI_COLETOR_LOCAL_TESTE) {
    return;
  }

  if (paiAtual == PAI_NENHUM || WiFi.status() != WL_CONNECTED) {
    if (agora - ultimaBuscaMs >= INTERVALO_BUSCA_MS) {
      escolherPai();
    }
  } else {
    // Se estamos usando outro roteador como ponte (2 saltos) e o bloqueio manual não está ativo,
    // verifica periodicamente se o Coletor direto voltou ao alcance para voltar ao caminho mais curto (1 salto)!
    if (paiAtual == PAI_OUTRO_ROTEADOR && !bloquearColetor &&
        (agora - ultimoTesteColetorDiretoMs >= INTERVALO_VOLTAR_COLETOR_MS)) {
      ultimoTesteColetorDiretoMs = agora;
#if MODO_TESTE_SEM_PC && (ROUTER_NUM == 1)
      escolherPai();
      return;
#else
      int n = WiFi.scanNetworks();
      bool coletorVisivel = false;
      int rssiColetor = -1000;
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == String(SSID_COLETOR)) {
          coletorVisivel = true;
          rssiColetor = WiFi.RSSI(i);
          break;
        }
      }
      WiFi.scanDelete();

      if (coletorVisivel && rssiColetor > -85) {
        Serial.printf("[%s] Coletor direto (%s, %d dBm) enxergado novamente! Tentando voltar para rota direta...\n",
                      MEU_ID, SSID_COLETOR, rssiColetor);
        if (!tentarAssociar(SSID_COLETOR, PAI_COLETOR)) {
          // Se ainda não conseguiu falar com o Coletor, reconecta rapidamente ao roteador anterior
          escolherPai();
        }
        return;
      }
#endif
    }

    if (agora - ultimoPingMs >= INTERVALO_PING_MS) {
      ultimoPingMs = agora;
      if (testarCaminhoAteColetorNoPai(paiAtual)) {
        falhasPai = 0;
      } else {
        falhasPai++;
        Serial.printf("[%s] Falha verificando caminho ate Coletor via %s (%d/%d)\n",
                      MEU_ID, ssidPaiAtual.c_str(), falhasPai, MAX_FALHAS_PAI);
        if (falhasPai >= MAX_FALHAS_PAI) {
          Serial.printf("[%s] Pai %s perdeu caminho ate o Coletor! Buscando outra rota...\n",
                        MEU_ID, ssidPaiAtual.c_str());
          escolherPai();
        }
      }
    }
  }
}

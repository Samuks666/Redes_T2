# Rede de Sensores Sem Fio (DEC7563) — Base Mínima para Arduino IDE

Esta estrutura já está organizada no padrão exigido pela **Arduino IDE** (cada `.ino` dentro de uma pasta de mesmo nome) e segue todas as regras do enunciado:
- **Modo infraestrutura IEEE 802.11 (`WIFI_AP_STA` nos roteadores, `WIFI_STA` nos sensores)**;
- **Sub-redes distintas nos roteadores** (`10.0.1.1/24` em `R1` e `10.0.2.1/24` em `R2` via `WiFi.softAPConfig`);
- **Encaminhamento pelo programa do grupo** usando UDP na porta `5000` (sem precisar instalar bibliotecas extras);
- **Prevenção de laço (`R1 <-> R2`)** e **recuperação automática** nos dois cenários de falha;
- **Compatibilidade dupla no sensor**: o código de `sensor/sensor.ino` compila tanto para **ESP32** quanto para **ESP8266** automaticamente.

---

## Estrutura de Arquivos

- [`sensor/sensor.ino`](sensor/sensor.ino): Código do Sensor (`S1` ou `S2`). Envia 1 mensagem/segundo numerada, mede o RTT (atraso de ida e volta), lê o `WiFi.RSSI()` do pai e troca automaticamente entre `R1` e `R2` se parar de receber `ACK`.
- [`roteador/roteador.ino`](roteador/roteador.ino): Código único para `R1` e `R2` (muda apenas `#define ROUTER_NUM 1` ou `2`). Recebe pacotes dos filhos, anexa seu ID no caminho e o `WiFi.RSSI()` lido do próprio pai, encaminha ao pai e devolve o `ACK`.
- [`coletor/coletor.py`](coletor/coletor.py): Programa do Coletor `C` para rodar no Linux, gravando o CSV e respondendo `ACK`.

---

## Modo 1: Testar SÓ com as 3 placas ESP32 (Sem Hotspot no PC)

Se você quiser testar imediatamente na bancada usando **apenas 1 Sensor + 2 Roteadores (`R1` e `R2`)**, sem configurar o Hotspot no Linux ainda:

1. Abra [`roteador/roteador.ino`](roteador/roteador.ino) na Arduino IDE e mude a linha 25 para:
   ```cpp
   #define MODO_TESTE_SEM_PC true
   ```
2. **Grave o Roteador 1 (`R1`)**:
   - Deixe `#define ROUTER_NUM 1` e faça o upload na primeira ESP32.
   - Neste modo de teste, `R1` atua como Roteador + Coletor final (imprimindo a linha CSV no Monitor Serial de `R1` a `115200` baud).
3. **Grave o Roteador 2 (`R2`)**:
   - Mude para `#define ROUTER_NUM 2` e faça o upload na segunda ESP32.
   - `R2` vai se associar automaticamente ao `R1` (`GRUPO_R1`) e manter o próprio AP (`GRUPO_R2`).
4. **Grave o Sensor (`S1`)**:
   - Abra [`sensor/sensor.ino`](sensor/sensor.ino) e faça o upload na terceira placa (ESP32 ou ESP8266).
   - Teste desligar `R1` ou `R2`, ou forçar `S1` a passar por `S1 -> R2 -> R1`!

---

## Modo 2: Topologia Completa do Trabalho (Com Coletor C no Linux)

1. **No computador Linux (Coletor C)**, crie o Ponto de Acesso Wi-Fi (substitua `wlan0` pelo nome da sua interface vista em `ip link show`):
   ```bash
   nmcli device wifi hotspot ifname wlan0 ssid GRUPO_COLETOR password senha1234 band bg channel 6
   ```
   *(O IP do computador nessa rede será `10.42.0.1`. Caso o firewall bloqueie a porta UDP 5000, libere com `sudo ufw allow 5000/udp`).*

2. **Rode o Coletor em Python**:
   ```bash
   python3 coletor/coletor.py
   ```

3. **Grave `R1` e `R2`** com `#define MODO_TESTE_SEM_PC false`:
   - Na placa R1: `#define ROUTER_NUM 1`
   - Na placa R2: `#define ROUTER_NUM 2`

4. **Como testar os 2 cenários de falha (Seção 2.3 item 7)**:
   - **Cenário 1 (Cai um roteador)**: Desligue da tomada/USB o roteador pelo qual `S1` está enviando. Após 3 pacotes sem `ACK` (~3 s), `S1` escaneia e migra sozinho para o outro roteador.
   - **Cenário 2 (Cai o caminho direto de um roteador até o Coletor)**: No Monitor Serial do roteador (ex.: `R1`), digite `f` e pressione Enter. O roteador para de ouvir o Coletor `C`, detecta a perda de `ACK`/heartbeat sozinho e passa a usar `R2` como pai (`S1 -> R1 -> R2 -> C`). Digite `f` novamente para restaurar.
   - **Scan de ocupação do canal (Seção 4.2 item 4)**: Digite `s` no Monitor Serial do roteador para listar todas as redes Wi-Fi vizinhas e seus canais/RSSI.

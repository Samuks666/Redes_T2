#!/usr/bin/env python3
"""
Trabalho Prático - Coletor C (Computador Linux)
Escuta mensagens UDP na porta 5000, responde ACK pelo mesmo caminho,
responde testes de rota (PING / CHECK_COLETOR) e grava CSV.
"""

import csv
import os
import socket
import sys
from datetime import datetime

UDP_IP = "0.0.0.0"
UDP_PORT = 5000
CSV_FILE = sys.argv[1] if len(sys.argv) > 1 else "medicoes.csv"


def main():
    novo_arquivo = not os.path.exists(CSV_FILE)
    f_csv = open(CSV_FILE, mode="a", newline="", encoding="utf-8")
    writer = csv.writer(f_csv)

    if novo_arquivo:
        writer.writerow([
            "timestamp_chegada",
            "epoch_s",
            "sensor_origem",
            "seq",
            "rtt_anterior_ms",
            "caminho",
            "potencias_dbm",
        ])
        f_csv.flush()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_IP, UDP_PORT))

    print(f"=== COLETOR C INICIADO NA PORTA UDP {UDP_PORT} ===")
    print(f"Gravando dados em: {CSV_FILE}\n")

    try:
        while True:
            data, addr = sock.recvfrom(512)
            msg = data.decode("utf-8", errors="ignore").strip()
            agora = datetime.now()
            ts_str = agora.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
            epoch_s = f"{agora.timestamp():.3f}"

            # Responde aos testes de rota ativa dos roteadores e sensores
            if msg.startswith("PING") or msg.startswith("CHECK_COLETOR"):
                sock.sendto(b"ROTA_OK;C", addr)
                continue

            # Formato esperado: DATA;origem;seq;rtt_ant;caminho;potencias
            partes = msg.split(";")
            if len(partes) >= 6 and partes[0] == "DATA":
                _, origem, seq, rtt_ant, caminho, potencias = partes[:6]
                caminho_completo = f"{caminho}->C"

                # Envia ACK de volta ao roteador que entregou (que devolverá até o sensor)
                ack = f"ACK;{origem};{seq}".encode("utf-8")
                sock.sendto(ack, addr)

                # Registra no CSV
                writer.writerow([
                    ts_str,
                    epoch_s,
                    origem,
                    seq,
                    rtt_ant,
                    caminho_completo,
                    potencias,
                ])
                f_csv.flush()

                print(
                    f"[{ts_str}] {origem} | seq={seq:>4} | RTT_ant={rtt_ant:>4} ms "
                    f"| caminho={caminho_completo:<14} | RSSI=[{potencias}] dBm"
                )
    except KeyboardInterrupt:
        print("\nEncerrando Coletor...")
    finally:
        f_csv.close()
        sock.close()


if __name__ == "__main__":
    main()

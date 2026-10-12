#!/usr/bin/env python3
"""
FarmTech Solutions - Fase 2 | "Ir além": integração Python + API pública (OpenWeather)

Consulta a previsão das próximas 24 h (8 intervalos de 3 h) e calcula a chuva
esperada em mm. O resultado é impresso no formato do comando do ESP32
(ex.: R4.5), que deve ser digitado no Monitor Serial do Wokwi.

Uso:
    export OPENWEATHER_API_KEY="sua_chave"          # Windows: set OPENWEATHER_API_KEY=...
    python clima_openweather.py                      # usa a fazenda padrão
    python clima_openweather.py --lat -12.54 --lon -55.72
    python clima_openweather.py --demo               # sem internet / sem chave
    python clima_openweather.py --porta rfc2217://localhost:4000   # envio automático (opcional)

Só usa a biblioteca padrão (o envio automático exige:  pip install pyserial).
"""

import argparse
import csv
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime

URL_PREVISAO = "https://api.openweathermap.org/data/2.5/forecast"
CHUVA_LIMITE_MM = 2.0  # mesmo limite usado no sketch.ino

# Fazenda fictícia (região produtora de soja). Troque pelas coordenadas do grupo.
LAT_PADRAO = -12.5425
LON_PADRAO = -55.7211


def buscar_previsao(lat, lon, chave, intervalos=8):
    """Chama a API e devolve o JSON da previsão (8 x 3 h = 24 h)."""
    params = urllib.parse.urlencode({
        "lat": lat, "lon": lon, "appid": chave,
        "units": "metric", "lang": "pt_br", "cnt": intervalos,
    })
    try:
        with urllib.request.urlopen(f"{URL_PREVISAO}?{params}", timeout=15) as resp:
            return json.load(resp)
    except urllib.error.HTTPError as e:
        if e.code == 401:
            sys.exit("Erro 401: chave da API inválida (chaves novas podem levar algumas horas para ativar).")
        sys.exit(f"Erro HTTP {e.code} ao consultar a API.")
    except urllib.error.URLError as e:
        sys.exit(f"Sem conexão com a API: {e.reason}. Use --demo para testar offline.")


def dados_demo():
    """Previsão fictícia (formato idêntico ao da API) para testes offline."""
    base = [
        ("2026-10-12 00:00:00", 22.1, "céu limpo", 0.0, 0.05),
        ("2026-10-12 03:00:00", 21.0, "nublado", 0.0, 0.20),
        ("2026-10-12 06:00:00", 20.5, "chuva leve", 1.2, 0.55),
        ("2026-10-12 09:00:00", 24.3, "chuva moderada", 3.8, 0.80),
        ("2026-10-12 12:00:00", 28.7, "chuva moderada", 2.5, 0.75),
        ("2026-10-12 15:00:00", 30.2, "nublado", 0.0, 0.30),
        ("2026-10-12 18:00:00", 27.4, "nublado", 0.0, 0.15),
        ("2026-10-12 21:00:00", 24.9, "céu limpo", 0.0, 0.05),
    ]
    lista = []
    for dt_txt, temp, desc, mm, pop in base:
        item = {"dt_txt": dt_txt, "main": {"temp": temp},
                "weather": [{"description": desc}], "pop": pop}
        if mm > 0:
            item["rain"] = {"3h": mm}
        lista.append(item)
    return {"list": lista}


def analisar(dados):
    """Resume a previsão: total, chuva esperada (mm x probabilidade) e probabilidade máxima."""
    linhas = []
    total_mm = esperado_mm = pop_max = 0.0
    temps = []

    for item in dados.get("list", []):
        mm = float(item.get("rain", {}).get("3h", 0.0))  # campo ausente = sem chuva
        pop = float(item.get("pop", 0.0))                # probabilidade de precipitação (0..1)
        temp = float(item["main"]["temp"])
        total_mm += mm
        esperado_mm += mm * pop
        pop_max = max(pop_max, pop)
        temps.append(temp)
        linhas.append((item["dt_txt"], temp, item["weather"][0]["description"], mm, pop))

    return {
        "linhas": linhas,
        "total_mm": round(total_mm, 1),
        "esperado_mm": round(esperado_mm, 1),
        "pop_max": pop_max,
        "temp_min": min(temps) if temps else None,
        "temp_max": max(temps) if temps else None,
    }


def imprimir_relatorio(res):
    print("\nPrevisão das próximas 24 h")
    print("-" * 68)
    print(f"{'Data/hora':<20}{'Temp °C':>8}  {'Condição':<18}{'mm/3h':>6}{'Prob.':>7}")
    for dt_txt, temp, desc, mm, pop in res["linhas"]:
        print(f"{dt_txt:<20}{temp:>8.1f}  {desc:<18}{mm:>6.1f}{pop:>6.0%}")
    print("-" * 68)
    print(f"Chuva total prevista : {res['total_mm']:.1f} mm")
    print(f"Chuva esperada (mm x prob.): {res['esperado_mm']:.1f} mm   <- valor enviado ao ESP32")
    print(f"Probabilidade máxima : {res['pop_max']:.0%}")

    if res["esperado_mm"] >= CHUVA_LIMITE_MM:
        print(f"\nRecomendação: SUSPENDER a irrigação (>= {CHUVA_LIMITE_MM} mm esperados).")
    else:
        print(f"\nRecomendação: irrigação liberada (< {CHUVA_LIMITE_MM} mm esperados).")


def salvar_historico(res, lat, lon, arquivo="historico_clima.csv"):
    novo = not os.path.exists(arquivo)
    with open(arquivo, "a", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        if novo:
            w.writerow(["consulta", "lat", "lon", "total_mm", "esperado_mm", "pop_max"])
        w.writerow([datetime.now().isoformat(timespec="seconds"), lat, lon,
                    res["total_mm"], res["esperado_mm"], f"{res['pop_max']:.2f}"])


def enviar_serial(porta, comando, baud=115200):
    """Envio automático ao ESP32 (placa real ou simulador com servidor RFC2217)."""
    try:
        import serial  # pyserial
    except ImportError:
        sys.exit("Instale o pyserial para envio automático:  pip install pyserial")
    with serial.serial_for_url(porta, baudrate=baud, timeout=2) as s:
        s.write((comando + "\n").encode())
        print(f"Comando '{comando}' enviado para {porta}.")


def main():
    ap = argparse.ArgumentParser(description="Previsão de chuva (OpenWeather) para o ESP32 da FarmTech")
    ap.add_argument("--lat", type=float, default=LAT_PADRAO)
    ap.add_argument("--lon", type=float, default=LON_PADRAO)
    ap.add_argument("--api-key", default=os.environ.get("OPENWEATHER_API_KEY"))
    ap.add_argument("--demo", action="store_true", help="usa dados fictícios (offline)")
    ap.add_argument("--porta", help="porta serial / URL RFC2217 para envio automático (opcional)")
    ap.add_argument("--salvar", action="store_true", help="grava o resultado em historico_clima.csv")
    args = ap.parse_args()

    if args.demo:
        dados = dados_demo()
    else:
        if not args.api_key:
            sys.exit("Defina OPENWEATHER_API_KEY (ou use --api-key / --demo).")
        dados = buscar_previsao(args.lat, args.lon, args.api_key)

    res = analisar(dados)
    imprimir_relatorio(res)
    if args.salvar:
        salvar_historico(res, args.lat, args.lon)

    comando = f"R{res['esperado_mm']:.1f}"
    print(f"\n>>> Digite no Monitor Serial do Wokwi:  {comando}")
    if args.porta:
        enviar_serial(args.porta, comando)


if __name__ == "__main__":
    main()

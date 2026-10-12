# FarmTech Solutions — Fase 2: Irrigação Inteligente com ESP32

Sistema de irrigação automatizado simulado no **Wokwi (ESP32)**, com integração Python (API OpenWeather) e análise estatística em R.

## 1. Estrutura

```
farmtech-fase2/
├── wokwi/
│   ├── sketch.ino        # firmware do ESP32 (C/C++)
│   └── diagram.json      # circuito do Wokwi (sem libraries.txt: nenhuma biblioteca)
├── python/
│   └── clima_openweather.py   # previsão de chuva (API pública)
├── r/
│   └── analise_irrigacao.R    # análise estatística
└── README.md
```

## 2. Cultura escolhida: Soja

A soja foi usada como exemplo (ajuste para a cultura da sua Fase 1, se for outra). Os valores abaixo são **didáticos**; confirme-os nas publicações da Embrapa Soja antes de citá-los.

| Variável | Valor adotado | Justificativa |
|---|---|---|
| pH ideal | 6,0 a 6,8 | Faixa em que P, K e micronutrientes ficam mais disponíveis |
| Umidade para ligar | < 60 % | Abaixo disso a planta começa a sofrer déficit hídrico |
| Umidade para desligar | ≥ 75 % | Evita encharcamento (histerese de 15 p.p.) |
| Umidade crítica | < 30 % | Emergência: irriga mesmo com restrições |
| Nitrogênio (N) | informativo | A soja fixa N pelo *Bradyrhizobium*; N não bloqueia a irrigação |
| Fósforo (P) e Potássio (K) | obrigatórios | Essenciais para a cultura |
| Chuva prevista | ≥ 2 mm em 24 h | Suspende a irrigação para economizar água |

## 3. Circuito (Wokwi)

| Componente | Pino ESP32 | Papel na simulação |
|---|---|---|
| Botão verde **N** | GPIO 25 (e GND) | Nível de Nitrogênio |
| Botão verde **P** | GPIO 26 (e GND) | Nível de Fósforo |
| Botão verde **K** | GPIO 27 (e GND) | Nível de Potássio |
| LDR (módulo) | GPIO 34 (AO), 3V3, GND | pH do solo (0 a 14) |
| DHT22 | GPIO 15 (SDA), 3V3, GND | Umidade do solo |
| Relé azul | GPIO 13 (IN), VIN, GND | Bomba d'água |
| LED azul + 220 Ω | GPIO 4 | Indicador visual da bomba |


<img width="921" height="583" alt="Image" src="https://github.com/user-attachments/assets/0b1b570c-d665-4aa2-8d57-e86883e17d6f" />
<img width="1415" height="1083" alt="Image" src="https://github.com/user-attachments/assets/ee78360a-1409-410c-8165-2cbd187793a7" />

## 4. Lógica de decisão da bomba

Avaliada nesta ordem (a primeira regra que se aplica vence):

| # | Condição | Bomba | Motivo |
|---|---|---|---|
| 1 | DHT22 com falha (NaN) | OFF | Fail-safe |
| 2 | Umidade ≥ 75 % | OFF | Solo úmido |
| 3 | Bomba desligada e umidade ≥ 60 % | OFF | Umidade adequada (histerese) |
| 4 | Umidade < 30 % | **ON** | Emergência hídrica |
| 5 | Chuva prevista ≥ 2 mm | OFF | Economia de água |
| 6 | pH fora de 6,0–6,8 | OFF | Alerta: corrigir solo (calagem) |
| 7 | P ou K deficiente | OFF | Alerta: adubar antes de irrigar |
| 8 | Demais casos (solo seco, pH e P/K ok) | **ON** | Irrigação com retorno agronômico |

**Raciocínio:** irrigar solo seco com pH inadequado ou P/K deficientes desperdiça água, pois a planta absorve mal os nutrientes. O sistema avisa o problema em vez de irrigar. A única exceção é a emergência hídrica, em que a sobrevivência da planta vem primeiro.

## 5. Como testar (cenários no Monitor Serial)

Ajuste o slider de umidade do DHT22 e o brilho do LDR. Lembre-se: **ao mudar N, P ou K, mexa também no LDR** para simular a alteração do pH.

| Cenário | Ação no Wokwi | Resultado esperado |
|---|---|---|
| Solo seco, tudo ok | Umidade 45 %, LDR em pH ~6,4, P=1 e K=1 | Bomba **LIGADA** |
| Umidade adequada | Umidade 65 % | Desligada |
| pH ácido | Umidade 45 %, LDR em pH ~5 | Desligada (alerta de calagem) |
| K deficiente | Umidade 45 %, clicar K até ficar deficiente | Desligada (alerta de adubação) |
| Chuva prevista | Digitar `R5` no Monitor Serial | Desligada (chuva) |
| Emergência | Umidade 20 % | Ligada, mesmo com pH ruim e chuva |
| Histerese | Com bomba ligada, subir a umidade a 70 % | Continua ligada; desliga em ≥ 75 % |

> O DHT22 do Wokwi não reage à irrigação: você simula o solo molhando ao subir o slider manualmente.

## 6. Python + OpenWeather

```bash
export OPENWEATHER_API_KEY="sua_chave"      # chave gratuita em openweathermap.org
python python/clima_openweather.py          # fazenda padrão
python python/clima_openweather.py --lat -12.54 --lon -55.72
python python/clima_openweather.py --demo   # sem chave, dados fictícios
```

O script usa a previsão de 5 dias/3 h, soma os próximos 24 h e calcula a **chuva esperada = mm × probabilidade**. Ele imprime um comando como `R5.6`; digite-o no Monitor Serial do Wokwi e o ESP32 atualiza a variável `chuvaPrevistaMm` e reavalia a decisão na hora (via `Serial.available()` / `Serial.read()`).

O script usa só a biblioteca padrão do Python. Envio automático (opcional, o único caso que exige instalar algo): `--porta rfc2217://localhost:4000` com `pip install pyserial`. Isso depende do recurso de serial remota do Wokwi para VS Code, que pode exigir plano pago; se não funcionar, use o envio manual acima.

## 7. Análise em R

1. Rode a simulação e copie as linhas que começam com `CSV,` do Monitor Serial para `r/dados_irrigacao.csv` (pode colar a saída bruta; o script filtra).
2. Execute `Rscript r/analise_irrigacao.R` (só R base, nenhum pacote extra). Sem o arquivo, o script usa dados simulados de demonstração.

O que ele faz: estatística descritiva e correlações; **teste t unilateral** (H0: umidade média ≥ 60 % vs H1: < 60 %, α = 5 %) sobre as últimas 10 leituras; **regressão linear** da tendência de secagem; combinação com as regras de pH, P, K e chuva; comparação (tabela de concordância) entre a decisão do ESP32 e a do R; e o gráfico `grafico_irrigacao.png`.


link do vídeo demonstrativo: https://youtu.be/oLiSLbZuDRY

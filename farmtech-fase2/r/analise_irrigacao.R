# =============================================================================
# FarmTech Solutions - Fase 2 | "Ir além": análise estatística em R
#
# Lê os dados gerados pelo ESP32 (linhas "CSV,..." copiadas do Monitor Serial
# para o arquivo dados_irrigacao.csv) e decide, com apoio estatístico, se a
# bomba de irrigação (relé azul) deve ficar ligada.
#
# Métodos:
#   1) Estatística descritiva e correlação
#   2) Teste t unilateral: a umidade média recente está ABAIXO do limiar?
#   3) Regressão linear: tendência de secagem e tempo até o limiar
#   4) Regras agronômicas (pH, P, K, chuva) combinadas ao resultado estatístico
#   5) Comparação entre a decisão do ESP32 e a recomendação do R
#
# Só usa o R base (nenhum pacote extra). Execute:  Rscript analise_irrigacao.R
# =============================================================================

# ------------------------- Parâmetros (iguais aos do sketch.ino) -------------
ARQUIVO        <- "dados_irrigacao.csv"
UMID_LIGA      <- 60     # % abaixo do qual o solo é considerado seco
UMID_CRITICA   <- 30     # % de emergência hídrica
PH_MIN         <- 6.0
PH_MAX         <- 6.8
CHUVA_LIMITE   <- 2.0    # mm
ALFA           <- 0.05   # nível de significância do teste t
JANELA         <- 10     # nº de leituras recentes usadas na análise
COLUNAS        <- c("tempo_s", "umidade", "ph", "N", "P", "K", "chuva_mm", "bomba")

# ------------------------- 1. Carga dos dados --------------------------------

# Gera dados simulados (apenas para demonstração, se não houver arquivo real).
gerar_dados_simulados <- function(n = 80) {
  set.seed(42)
  umid <- numeric(n); ph <- numeric(n); bomba <- integer(n)
  N <- rbinom(n, 1, 0.6); P <- rbinom(n, 1, 0.8); K <- rbinom(n, 1, 0.8)
  chuva <- ifelse(runif(n) < 0.15, round(runif(n, 0, 8), 1), 0)
  umid[1] <- 72; ph[1] <- 6.4; b <- 0
  for (i in 1:n) {
    if (i > 1) {
      # solo seca ~1,2 %/leitura; irrigação recupera ~3 %/leitura
      umid[i] <- umid[i - 1] - 1.2 + ifelse(b == 1, 3, 0) + rnorm(1, 0, 0.6)
      umid[i] <- min(max(umid[i], 5), 95)
      ph[i]   <- min(max(ph[i - 1] + rnorm(1, 0, 0.12), 4.5), 8.5)
    }
    ph_ok <- ph[i] >= PH_MIN && ph[i] <= PH_MAX
    if (umid[i] >= 75) b <- 0
    else if (umid[i] < UMID_CRITICA) b <- 1
    else if (b == 0 && umid[i] >= UMID_LIGA) b <- 0
    else if (chuva[i] >= CHUVA_LIMITE) b <- 0
    else b <- as.integer(ph_ok && P[i] == 1 && K[i] == 1)
    bomba[i] <- b
  }
  data.frame(tempo_s = (0:(n - 1)) * 2, umidade = round(umid, 1), ph = round(ph, 2),
             N = N, P = P, K = K, chuva_mm = chuva, bomba = bomba)
}

carregar_dados <- function(arquivo) {
  if (!file.exists(arquivo)) {
    message("Arquivo '", arquivo, "' não encontrado: usando DADOS SIMULADOS (demonstração).")
    return(gerar_dados_simulados())
  }
  linhas <- readLines(arquivo, warn = FALSE)
  if (any(grepl("^CSV,", linhas))) {           # saída bruta do Monitor Serial
    linhas <- sub("^CSV,", "", linhas[grepl("^CSV,", linhas)])
  }
  linhas <- linhas[nzchar(trimws(linhas)) & !grepl("tempo_s", linhas)]   # remove cabeçalho
  read.csv(text = paste(linhas, collapse = "\n"), header = FALSE, col.names = COLUNAS)
}

dados <- carregar_dados(ARQUIVO)
cat("\n==== 1. DADOS CARREGADOS ====\n")
cat("Leituras:", nrow(dados), "\n")
print(head(dados, 5))

# ------------------------- 2. Estatística descritiva -------------------------
cat("\n==== 2. ESTATÍSTICA DESCRITIVA ====\n")
cat("\nUmidade do solo (%):\n"); print(summary(dados$umidade))
cat("Desvio-padrão da umidade:", round(sd(dados$umidade), 2), "\n")
cat("\npH (via LDR):\n");        print(summary(dados$ph))
cat("\nUmidade média com bomba LIGADA x DESLIGADA:\n")
print(round(tapply(dados$umidade, dados$bomba, mean), 1))
cat("\nProporção de leituras com nutriente adequado:\n")
print(round(colMeans(dados[, c("N", "P", "K")]), 2))
cat("\nMatriz de correlação (umidade, pH, chuva, bomba):\n")
print(round(suppressWarnings(cor(dados[, c("umidade", "ph", "chuva_mm", "bomba")])), 2))  # NA = coluna constante

# ------------------------- 3. Recomendação estatística -----------------------
# Analisa uma janela de leituras e devolve a recomendação para a bomba.
recomendar <- function(w) {
  u   <- w$umidade
  ult <- w[nrow(w), ]

  # Teste t unilateral: H0: média >= limiar  vs  H1: média < limiar (solo seco)
  p_valor <- tryCatch(
    t.test(u, mu = UMID_LIGA, alternative = "less")$p.value,
    error = function(e) NA_real_            # dados constantes -> teste inviável
  )
  seco <- if (!is.na(p_valor)) p_valor < ALFA else mean(u) < UMID_LIGA

  # Regressão linear: tendência da umidade (pontos percentuais por minuto)
  inclinacao <- NA_real_
  if (nrow(w) >= 3 && length(unique(w$tempo_s)) > 1) {
    inclinacao <- unname(coef(lm(umidade ~ tempo_s, data = w))[2]) * 60
  }

  ph_ok  <- ult$ph >= PH_MIN && ult$ph <= PH_MAX
  pk_ok  <- ult$P == 1 && ult$K == 1
  chuva  <- ult$chuva_mm >= CHUVA_LIMITE

  if (ult$umidade < UMID_CRITICA) {
    irrigar <- TRUE;  motivo <- "EMERGÊNCIA: umidade crítica"
  } else if (!seco) {
    irrigar <- FALSE; motivo <- "Sem evidência estatística de solo seco"
  } else if (chuva) {
    irrigar <- FALSE; motivo <- "Chuva prevista: suspender"
  } else if (!ph_ok) {
    irrigar <- FALSE; motivo <- "Solo seco, mas pH fora da faixa (corrigir solo)"
  } else if (!pk_ok) {
    irrigar <- FALSE; motivo <- "Solo seco, mas P/K deficientes (adubar)"
  } else {
    irrigar <- TRUE;  motivo <- "Solo estatisticamente seco com pH e P/K adequados"
  }

  list(irrigar = irrigar, motivo = motivo, p_valor = p_valor,
       media = mean(u), inclinacao = inclinacao)
}

cat("\n==== 3. RECOMENDAÇÃO PARA AS ÚLTIMAS", JANELA, "LEITURAS ====\n")
janela <- tail(dados, JANELA)
rec <- recomendar(janela)
cat("Umidade média na janela :", round(rec$media, 1), "%  (limiar:", UMID_LIGA, "%)\n")
cat("p-valor do teste t      :", ifelse(is.na(rec$p_valor), "n/d (dados constantes)",
                                         format.pval(rec$p_valor, digits = 3)), "\n")
if (!is.na(rec$inclinacao)) {
  cat("Tendência da umidade    :", round(rec$inclinacao, 2), "p.p. por minuto\n")
  if (rec$inclinacao < 0 && rec$media > UMID_LIGA) {
    min_ate <- (rec$media - UMID_LIGA) / abs(rec$inclinacao)
    cat("Tempo estimado até o limiar:", round(min_ate, 1), "min\n")
  }
}
cat("\n>>> RECOMENDAÇÃO DO R:", ifelse(rec$irrigar, "LIGAR a bomba", "MANTER a bomba DESLIGADA"), "\n")
cat("    Motivo:", rec$motivo, "\n")

# ------------------------- 4. Concordância ESP32 x R -------------------------
cat("\n==== 4. DECISÃO DO ESP32 x RECOMENDAÇÃO DO R (janela móvel) ====\n")
if (nrow(dados) >= JANELA) {
  idx <- JANELA:nrow(dados)
  rec_r <- sapply(idx, function(i) as.integer(recomendar(dados[(i - JANELA + 1):i, ])$irrigar))
  tab <- table(ESP32 = dados$bomba[idx], R = rec_r)
  print(tab)
  cat("Concordância:", round(100 * sum(diag(tab)) / sum(tab), 1), "%\n")
  cat("(Diferenças são esperadas: o ESP32 decide por leitura instantânea com histerese;\n",
      " o R decide por evidência estatística em uma janela de leituras.)\n")
} else {
  cat("Dados insuficientes para a janela móvel (mínimo:", JANELA, "leituras).\n")
}

# ------------------------- 5. Gráficos ---------------------------------------
png("grafico_irrigacao.png", width = 1000, height = 700, res = 110)
par(mfrow = c(2, 1), mar = c(4, 4, 3, 1))

plot(dados$tempo_s, dados$umidade, type = "l", lwd = 2, col = "steelblue",
     xlab = "Tempo (s)", ylab = "Umidade do solo (%)", main = "Umidade do solo e acionamento da bomba")
abline(h = UMID_LIGA, col = "red", lty = 2)
abline(h = UMID_CRITICA, col = "darkred", lty = 3)
pts <- dados[dados$bomba == 1, ]
points(pts$tempo_s, pts$umidade, pch = 16, col = "darkgreen")
legend("topright", bty = "n", cex = 0.8,
       legend = c("Umidade", "Limiar (60%)", "Crítica (30%)", "Bomba ligada"),
       col = c("steelblue", "red", "darkred", "darkgreen"),
       lty = c(1, 2, 3, NA), pch = c(NA, NA, NA, 16), lwd = c(2, 1, 1, NA))

plot(dados$tempo_s, dados$ph, type = "l", lwd = 2, col = "purple",
     xlab = "Tempo (s)", ylab = "pH (LDR)", main = "pH do solo e faixa ideal da soja")
rect(min(dados$tempo_s), PH_MIN, max(dados$tempo_s), PH_MAX,
     col = rgb(0, 0.6, 0, 0.15), border = NA)
dev.off()
cat("\nGráfico salvo em grafico_irrigacao.png\n")

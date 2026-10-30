# DOC AI S3CAM — placa XiaoZhi para Projeto .DOC

## Primeiro marco incluído
- Alvo ESP32-S3.
- Caminho de rede/protocolo nativo XiaoZhi (`WifiBoard` + `Application`).
- Áudio I2S simplex: microfone MS3625 e amplificador MAX98357A.
- Botão BOOT: configuração Wi-Fi durante a inicialização; nos demais estados alterna o chat.
- API HTTP experimental para PCA9685, iniciada após evento de conexão Wi-Fi.

## Pinagem de áudio confirmada

| Sinal | GPIO |
|---|---:|
| Microfone BCLK/SCK | 1 |
| Microfone WS/LRCL | 2 |
| Microfone DOUT | 3 |
| MAX98357A BCLK | 41 |
| MAX98357A LRC | 42 |
| MAX98357A DIN | 47 |

## PCA9685 — pinagem e calibração inicial

| Sinal | Configuração |
|---|---|
| SDA | GPIO19 |
| SCL | GPIO20 |
| Endereço I²C | 0x40 |
| Frequência I²C | 100 kHz |
| Frequência PWM | 50 Hz |
| CH0 / CH3 | SG90 pernas, faixa inicial 1250–1750 µs |
| CH1 / CH2 | MG90S rodas, neutro 1705 µs |

**Importante sobre USB:** GPIO19/GPIO20 também são USB D-/D+ no ESP32-S3. Com I²C nesses pinos, não usar USB nativo simultaneamente. Para gravação e serial, usar o conversor USB-UART externo, como já feito nos testes.

Os limites de pulso das pernas são uma faixa inicial validada em testes, não uma garantia de segurança para qualquer montagem. Os pulsos das rodas são calibrações medidas para os motores deste robô.

## Painel web embutido

Depois de gravar o firmware com esta alteração e conectar o DOC ao Wi-Fi, abra `http://IP-DO-DOC/` em um celular ou computador conectado à mesma rede. A rota `/index.html` também serve o painel. A interface e a API usam a mesma porta HTTP (80), sem servidor separado no computador.

## API HTTP experimental

Quando a placa está conectada ao Wi-Fi, a API responde na porta 80 do endereço IP atribuído pelo roteador:

| Método | Rota | Corpo / função |
|---|---|---|
| GET | `/api/status` | Estado e valores atuais |
| POST | `/api/servo` | `{"channel":0,"pulse_us":1500}` ou canal 3 |
| POST | `/api/legs` | `{"left_us":1500,"right_us":1500}` |
| POST | `/api/wheels` | `{"left":"cw","right":"stop"}` |
| POST | `/api/stop` | Para os dois motores de roda |

Valores permitidos para rodas: `cw`, `ccw` e `stop`. Um watchdog solicita parada das rodas após 1,5 segundo sem atualização de comando. Isso é uma proteção de software, não um substituto para desligamento físico.

### Segurança e limitações conhecidas
- API ainda sem autenticação: usar somente em rede local confiável; não encaminhar a porta 80 para a internet.
- A interface responsiva é embutida no firmware para a placa DOC e servida nas rotas `/` e `/index.html`; o arquivo-fonte é `web_control/index.html` e a cópia usada na compilação é `main/boards/doc-ai-s3cam/web_control.html`.
- Ao abrir a página diretamente pelo IP da ESP32, o endereço da API é preenchido automaticamente com a mesma origem. Para usar a interface hospedada no computador, mantenha o IP do DOC no campo de conexão.
- O estado de calibração não é persistido em NVS.
- A API inicia o PCA9685 quando o Wi-Fi conecta. Se o driver I²C/PCA falhar, consultar o log serial antes de testar servos.
- O código ainda não foi compilado nesta sessão nem validado na placa. Não gravar no firmware principal antes de concluir build e revisão.

## Tela e câmera
A pinagem ST7735 1,77" e OV3660 está registrada em `config.h`, mas inicialização e testes são marcos separados.

## Compilação local
No repositório local XiaoZhi/ESP-IDF, selecionar a placa `doc-ai-s3cam` e compilar conforme o fluxo de build do projeto. A compilação e o teste físico continuam pendentes.


## TTP223 — teste isolado no GPIO48

- Sinal OUT do TTP223 ligado ao GPIO48.
- Alimentação recomendada para este teste: 3V3 e GND comum com a ESP32-S3.
- O firmware registra no log serial os eventos de início e fim do toque.
- O teste não altera o estado do chat e não comanda os servos.
- GPIO48 pode estar ligado ao LED RGB integrado em algumas revisões da placa. Observar possíveis interferências no LED e no sensor.
- Esta alteração está somente na branch `feature/ttp223-gpio48-functional`; não foi aplicada à `main`.
- A compilação e o teste físico ainda estão pendentes. Não gravar na placa antes da revisão e do build.

# Ponte Arduino / ESP32 para o SD Clone Tools

Com um Arduino ou ESP32 e um módulo de cartão micro SD, o app fala **direto com o cartão**, sem passar pelo leitor do computador. Isso permite:

- ler os registros internos do cartão (CID: fabricante, modelo, série e data; CSD: capacidade registrada e bits de proteção);
- ver se o cartão está **bloqueado por senha**;
- testar a gravação direto no cartão (grava 1 bloco e devolve o conteúdo original);
- desligar o bit de **proteção temporária** (TMP_WRITE_PROTECT), se ele estiver ligado. O bit de proteção permanente nunca é alterado.

## Do que você precisa

- Arduino Uno, Nano, Mega ou uma placa ESP32;
- módulo micro SD **com regulador e conversor de nível** (os módulos azuis comuns de 6 pinos). No ESP32 funciona qualquer módulo, porque ele já trabalha em 3,3 V;
- 6 fios jumper e o cabo USB da placa (cabo de dados, não só de carga).

## Ligação

| Módulo SD | Uno / Nano | Mega | ESP32 |
|---|---|---|---|
| CS   | D10 | D53 | GPIO 5  |
| MOSI | D11 | D51 | GPIO 23 |
| MISO | D12 | D50 | GPIO 19 |
| SCK  | D13 | D52 | GPIO 18 |
| VCC  | 5V  | 5V  | 3V3     |
| GND  | GND | GND | GND     |

## Como gravar o programa na placa

1. Instale a [IDE do Arduino](https://www.arduino.cc/en/software). Para ESP32, adicione o pacote de placas "esp32" da Espressif no Gerenciador de Placas.
2. Abra `sdtools_arduino/sdtools_arduino.ino`.
3. Em **Ferramentas**, escolha a placa e a porta.
4. Clique em **Carregar**.
5. Feche o Monitor Serial da IDE, se estiver aberto. O app precisa da porta livre.

## Como usar no app

1. Coloque o cartão no módulo e ligue a placa no USB.
2. No SD Clone Tools, abra **Proteção do cartão** e escolha **Arduino ou ESP32 conectado por USB**.
3. Clique em **Detectar** e confira a porta.
4. Clique em **Ler proteção**.
5. Se o resultado for **Proteção temporária ligada**, marque **Tentar desligar a proteção temporária** e clique em **Ler proteção** de novo.

## Como ler o resultado

| Resultado | O que significa |
|---|---|
| Proteção temporária ligada | O bloqueio pode ser desligado pela opção do app. |
| Proteção permanente ligada | O bit foi gravado de forma definitiva. Não sai. Faça backup. |
| Bloqueado por senha | Remova a senha no aparelho que a colocou (celular ou câmera). |
| Firmware travado em somente leitura | Não há bit de proteção ligado, mas a controladora recusa gravar. Não tem conserto por software. Faça backup. |
| Grava normalmente pelo Arduino | O cartão está bom. O problema é o leitor ou o adaptador do computador. |
| O cartão não iniciou no Arduino | Confira a ligação com um cartão bom. Se o cartão bom funciona e este não, a controladora deste está morta. |

## Protocolo (para quem quiser usar em outro programa)

115200 baud. Um comando por linha, e toda resposta termina com `END`.

- `SDT?`: responde `SDTOOLS 1 <placa>`.
- `INFO`: responde `INIT OK V2 HC` (ou `INIT ERR n`), `CID <hex>`, `CSD <hex>` e `STATUS <r1><r2>`.
- `WTEST`: responde `WTEST OK`, `WTEST REJECT <código> <r2>`, `WTEST IGNORED` ou `WTEST CORRUPT`, seguido de `RESTORE OK` ou `RESTORE ERR`.
- `CLRTMP`: responde `CLRTMP OK` (com o `CSD` novo), `CLRTMP NOTSET`, `CLRTMP PERM` ou `CLRTMP ERR n`.

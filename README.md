# SDCLONETOOLS

**Ferramenta gratuita e de código aberto para Windows para quem trabalha com cartões SD em gamesticks e consoles portáteis chineses.**

Quem monta, grava e conserta esses aparelhos (R36S e clones, M17, gamesticks e similares) vive com cartões SD: gravar o sistema, clonar o cartão que já está configurado, fazer backup, recuperar o que sumiu e, principalmente, descobrir se o cartão que veio na caixa é mesmo o que diz ser. O SDCLONETOOLS junta tudo isso em **um único `.exe`**, sem instalação, sem anúncios e sem dependências.

## Download

[**⬇ Baixar SDCloneTools.exe (Windows 64 bits)**](https://github.com/emersoncassis/SDCLONETOOLS/releases/latest/download/SDCloneTools.exe)

[Baixar SDCloneTools32.exe (Windows 32 bits)](https://github.com/emersoncassis/SDCLONETOOLS/releases/latest/download/SDCloneTools32.exe) · [Todas as versões](https://github.com/emersoncassis/SDCLONETOOLS/releases)

Arquivo único, sem instalação. Execute e aceite o pedido de administrador.

## Telas

O menu à esquerda tem as funções; a área à direita mostra a tarefa e, no fim, o resultado.

**Proteção do cartão**: dois modos separados, leitor interno do computador ou Arduino/ESP32.

![Proteção do cartão](docs/img/protecao.png)

**Diagnóstico**: o resultado vem como veredito (causa provável, se tem conserto, se dá para salvar os dados e o que fazer) e um mapa de leitura de 128 pontos do cartão.

![Veredito do Diagnóstico](docs/img/diagnostico-veredito.png)

## O que faz

| Função | Para que serve |
|---|---|
| **Diagnóstico** | Analisa o cartão sem apagar nada: tabela de partições, leitura em 128 pontos, teste opcional de gravação (grava 4 KB em 2 pontos e devolve o conteúdo original) e leitura do registro CID. Termina com um veredito claro: causa provável, se o cartão tem conserto, se dá para salvar os dados e o que fazer agora |
| **Proteção do cartão** | Lê os bits de proteção contra gravação (CSD) e o bloqueio por senha, para saber se o cartão está travado por proteção ou pelo firmware. Tem dois modos, descritos abaixo |
| **Recuperar e reparar** | Análise de partições, ver e copiar arquivos (inclusive ext4 de consoles), reconstruir partições perdidas sem apagar dados, procurar arquivos apagados (de uma imagem ou do cartão) e restaurar o cartão |
| **Autenticidade** | Descobre se o cartão é verdadeiro ou falso: marca, fabricante registrado, capacidade real e velocidade |
| **Verificar setores** | Leitura completa, comparação com uma imagem e teste de capacidade real |
| **Fazer backup** | Cartão para arquivo `.img`, com conferência opcional |
| **Gravar imagem** | Grava `.img`, `.iso` ou `.bin` no cartão, com verificação |
| **Clonar cartão** | Copia um cartão direto para outro |

### Proteção do cartão: dois modos

| | Leitor interno do computador | Arduino ou ESP32 por USB |
|---|---|---|
| Serve para | Slot de cartão SD do notebook/PC | Qualquer cartão micro SD, com um módulo de ~R$ 10 |
| Leitor USB | Não funciona (leitores USB não repassam comandos ao cartão) | Não depende do leitor do PC |
| O que faz | Só lê o CID, o CSD e o estado de bloqueio | Lê os registros, testa gravação direto no cartão e pode desligar a proteção **temporária** |
| Instalação | Nenhuma | Gravar o programa em [`arduino/`](arduino/LEIA.md) na placa |

No modo Arduino, o app lista as portas USB, marca as que parecem Arduino ou ESP32 e confirma que a placa tem o programa do SD Clone Tools antes de começar. O resultado diz se o bloqueio é proteção temporária, proteção permanente, senha ou **firmware travado em somente leitura** (o cartão recusa gravar mesmo sem nenhum bit ligado; não há conserto por software, só salvar os dados). Se o cartão grava normalmente direto no Arduino, o defeito está no leitor ou adaptador do computador. A ligação dos fios e o passo a passo estão em [arduino/LEIA.md](arduino/LEIA.md).

### Cartão de 128 GB aparecendo com poucos MB? Cartão "sem acesso"?

Use **Diagnóstico**, depois **Recuperar e reparar**. O app lê o cartão sem alterar nada e diz qual é o caso:

- **o próprio cartão informa pouca capacidade** (controlador com defeito ou cartão falso): não é partição, não tem conserto por software comum, e o app explica isso;
- **partições cobrindo só parte do cartão** (imagem pequena gravada em cartão grande): os dados estão intactos e o espaço pode ser recuperado;
- **partições Linux que o Windows não lê** (normal em R36S, ArkOS, JELOS): o app abre e copia esses arquivos;
- **tabela de partições apagada** (cartão "RAW"): o app encontra as partições e reconstrói a tabela sem apagar nada;
- **cartão que não aceita gravação** (erro de E/S 1117 ao formatar): firmware em modo somente leitura, comum em cartão desgastado. Leitura ainda funciona, então o app orienta a fazer o backup primeiro.

### Por que a Autenticidade importa

Cartão falso (um cartão de 8 GB que se anuncia como 128 GB, por exemplo) é comum em kits e aparelhos baratos. Ele grava e lê normalmente até o espaço real acabar, e depois corrompe os dados em silêncio. O SDCLONETOOLS:

- lê o **fabricante gravado no cartão** (registro CID) e compara com a marca da etiqueta (SanDisk, Samsung, Kingston, Lexar, Sony, Transcend, ADATA...);
- faz o **teste de capacidade real**: grava dados em todo o cartão e confere se voltam idênticos;
- compara a velocidade medida com a **classe anunciada** (Class 10, U1, U3, V30...).

> O chip de memória (NAND) em si não fica registrado no cartão, então nenhum software consegue dizer quem o fabricou. A ferramenta identifica o fabricante/etiqueta do cartão, e o teste de capacidade mostra se ele entrega o que promete.

## Download e uso rápido

1. [Baixe o `SDCloneTools.exe`](https://github.com/emersoncassis/SDCLONETOOLS/releases/latest/download/SDCloneTools.exe) (use o `SDCloneTools32.exe` apenas em Windows de 32 bits).
2. Execute e aceite o pedido de **administrador** (necessário para acessar o disco direto).
3. Escolha a função, o cartão e clique em **Iniciar**.

O guia completo, função por função, está em [docs/USO.md](docs/USO.md).

## Segurança

- O disco do sistema **nunca aparece** na lista, e por padrão só aparecem cartões e discos removíveis.
- Toda operação que grava mostra o disco escolhido e pede confirmação. **Confira o disco antes de confirmar.**
- **Teste com um cartão sem dados importantes antes de usar no dia a dia.** Este é um software novo.
- O Windows SmartScreen ou o antivírus podem alertar, porque o programa acessa o disco diretamente e não tem assinatura digital. Todo o código-fonte está neste repositório, e você pode compilar o seu próprio `.exe` (veja abaixo).

## Compilar

C puro com a API Win32, sem dependências. Precisa de Python e do compilador Zig (`pip install ziglang`); funciona a partir de Linux, macOS ou Windows:

```sh
./build.sh
```

Isso gera `SDCloneTools.exe` (64 bits) e `SDCloneTools32.exe`.

Testes automatizados (em Linux):

```sh
gcc -O1 -o test_core test_core.c && ./test_core   # recuperação de arquivos apagados
sudo python3 tests/run_tests.py                    # leitores de FAT/exFAT/ext4 e diagnóstico (imagens reais)
```

O segundo precisa de `dosfstools mtools exfatprogs exfat-fuse e2fsprogs fdisk`.

## Estrutura

| Arquivo | Conteúdo |
|---|---|
| `sdclone.c` | Interface Win32 e operações de disco |
| `arduino/` | Programa para Arduino/ESP32 e guia de ligação (modo Arduino da Proteção do cartão) |
| `core.h` | Motor de recuperação, padrão do teste de capacidade e tabela de fabricantes |
| `fsread.h` | Leitura somente leitura de FAT12/16/32, exFAT e ext2/3/4 |
| `diag.h` | Diagnóstico de MBR/GPT, partições perdidas e reconstrução do MBR |
| `repair.h` | Diagnóstico, reconstrução e navegador de arquivos na interface |
| `test_core.c`, `fstest.c`, `tests/` | Testes automatizados |
| `sdclone.rc`, `sdclone.manifest`, `sdclone.ico`, `mkicon.py` | Ícone e manifesto (exige administrador) |
| `build.sh` | Script de compilação |
| `docs/USO.md` | Guia de uso completo |

## Contribuir

Contribuições são muito bem-vindas: relatos de cartões testados, novos fabricantes na tabela, correções e traduções. Veja [CONTRIBUTING.md](CONTRIBUTING.md).

## Licença

[MIT](LICENSE). Use, modifique e distribua à vontade.

## Aviso

Este software acessa dispositivos de armazenamento diretamente e é fornecido **sem garantia de qualquer tipo**. Use por sua conta e risco e mantenha backup do que importa.

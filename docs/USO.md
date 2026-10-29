# SDCLONETOOLS — guia de uso completo

Aplicativo simples para Windows (um único `.exe`, sem instalação) para **copiar, gravar, verificar, autenticar e recuperar cartões SD**.

| Arquivo | Para quê |
|---|---|
| `SDCloneTools.exe` | Versão 64 bits (use esta) |
| `SDCloneTools32.exe` | Só para Windows de 32 bits |

## Antes de começar

- Requer **Windows 10 ou 11**.
- O app pede permissão de **administrador** ao abrir. É necessário para acessar o disco diretamente. Clique em **Sim**.
- O Windows SmartScreen ou o antivírus podem alertar, pois o programa não tem assinatura digital e acessa disco direto. Clique em **Mais informações → Executar assim mesmo**.
- **Teste primeiro com um cartão sem dados importantes.**
- Feche o Explorador de Arquivos e programas que estejam usando o cartão.
- O disco do sistema (onde está o Windows) **nunca aparece** na lista. Por padrão só aparecem cartões e discos removíveis.

## Como usar (passo a passo)

1. Insira o cartão e abra o `SDCloneTools.exe`.
2. Clique na função desejada no menu à esquerda.
3. Escolha o cartão na lista. Se ele não aparecer, clique em **Atualizar**.
4. Preencha o que a função pedir (arquivo de imagem, pasta etc.).
5. Clique no botão azul (o nome muda conforme a função, por exemplo **Analisar cartão** ou **Criar backup**) e confirme o aviso. Toda operação que apaga dados mostra o disco escolhido e pede confirmação.
6. Acompanhe a barra de progresso, a velocidade e o tempo restante. **Cancelar** interrompe a operação.
7. Ao final, no Diagnóstico e na Proteção do cartão, aparece um veredito; use **Ver registro técnico** para o registro completo e **Alterar opções** para voltar ao formulário. Nas outras funções o registro aparece direto na tela.

## As funções

### Diagnóstico
Primeira coisa a fazer com um cartão que dá problema. **Não apaga nada** (o teste de gravação devolve o conteúdo original).

- Lê a tabela de partições e reconhece o sistema de arquivos.
- Lê 128 pontos do cartão, do início ao fim, e marca no mapa: com dados, vazio, lento (mais de 1 s) ou ilegível.
- Com **Testar gravação** marcado, grava 4 KB em 2 pontos com o padrão invertido, confere e restaura. Isso separa "cartão só de leitura" de "cartão que perde o que grava".
- Lê o registro CID (fabricante e data) quando o cartão está em slot SD nativo.

O resultado é um veredito em quatro linhas: **causa provável**, **o cartão tem conserto?**, **dá para salvar os dados?** e **o que fazer agora**. Se couber, aparece um botão para o próximo passo (fazer backup, recuperar arquivos, reconstruir partições etc.).

![Veredito do Diagnóstico](img/diagnostico-veredito.png)

**Aba Conteúdo.** Depois da análise, o Diagnóstico também lê as pastas e arquivos do cartão (somente leitura; FAT, exFAT e ext2/3/4) e mostra na aba **Conteúdo**:
- quanto do cartão está ocupado, em uma barra por tipo de arquivo, com o que está livre e o que está fora das partições;
- os tipos: jogos (ROMs de emuladores), sistema e emulador (BIOS, saves, configurações, sistema Linux), música, vídeo, fotos, documentos, programas e compactados;
- as extensões mais comuns (`.gba`, `.mp3`, `.jpg`...), com quantidade e tamanho;
- o **tipo de uso** do cartão (jogos e emulação, console/game stick com sistema Linux, câmera, vídeos, música, documentos ou misto), pelas pastas (`roms`, `retroarch`, `DCIM`...) e pelas extensões;
- se o cartão **serve para esse uso**, juntando o veredito de saúde e, se você já rodou o **Desempenho** nesse cartão, a velocidade medida.

Cartões NTFS, criptografados ou com sistema desconhecido têm o conteúdo ignorado (o app avisa).

### Desempenho
Mede o cartão e diz para que ele presta. Não precisa de nenhum aparelho extra.
- **Leitura sequencial** em 3 regiões (início, meio e fim). Se uma região for bem mais lenta que as outras, é sinal de desgaste ou de cartão falso.
- **Acesso aleatório de 4 KB** em leitura (o que os jogos mais usam) e, com o teste de gravação, em gravação.
- **Gravação sequencial** (opcional, marcada por padrão): usa 32 MB no meio do cartão. O app copia o conteúdo original para a memória, grava, confere o que gravou e **devolve o original e confere de novo**, mesmo se você cancelar. Não retire o cartão durante o teste.
- O gráfico mostra a velocidade ao longo do teste. Quedas bruscas na gravação indicam cartão ruim.

O resultado diz: velocidade medida, acesso aleatório (bom, médio ou fraco), classe estimada (Class 10 / U1, U3 / V30, V60, V90, pela gravação sequencial) e para que serve:

| Resultado | Significado |
|---|---|
| Game stick: bom | Leitura de 40 MB/s ou mais e acesso aleatório rápido |
| Game stick: serve para jogos leves | Leitura acima de 20 MB/s; pode travar em PSP, Dreamcast e jogos pesados |
| Foto e vídeo: Full HD / 4K | Gravação de 10 MB/s ou mais (Full HD) e 30 MB/s ou mais (4K) |
| Cartão genérico ou instável | Erros, leitura muito desigual, quedas bruscas ou velocidade muito baixa: não guarde nada que não possa perder |

Os números são medidos **neste leitor**, uma operação por vez; leitores USB lentos mostram menos do que o cartão entrega, e os limites são aproximados, não os da especificação oficial (A1/A2). Compare cartões testados no mesmo leitor.

### Proteção do cartão
Descobre **por que o cartão não deixa gravar**. Escolha o modo no topo da tela.

**Leitor interno do computador.** Só funciona no slot de cartão SD do notebook ou PC, e só se o driver permitir. Usa o canal de comandos SD do Windows para ler o CSD e o CID e ver se o cartão está bloqueado por senha. Não altera nada. Se o cartão estiver num leitor USB, o app avisa e indica o modo Arduino.

**Arduino ou ESP32 conectado por USB.** O app lista as portas USB, marca as que parecem Arduino/ESP32 e confirma que a placa tem o programa do SD Clone Tools. Fala direto com o cartão e, além de ler os registros:
- testa a gravação em 1 bloco e devolve o conteúdo original;
- com **Tentar desligar a proteção temporária** marcado (o app pede confirmação), limpa o bit TMP_WRITE_PROTECT. A proteção **permanente** nunca é alterada.

Ligação dos fios, como gravar o programa na placa e a tabela de resultados: [arduino/LEIA.md](../arduino/LEIA.md).

| Resultado | Significado |
|---|---|
| Proteção temporária ligada | Pode ser desligada pelo modo Arduino |
| Proteção permanente ligada | Definitiva. Faça backup |
| Bloqueado por senha | Remova a senha no aparelho que a colocou |
| Firmware travado em somente leitura | Nenhum bit ligado, mas a controladora recusa gravar. Sem conserto por software. Faça backup |
| Grava normalmente pelo Arduino | O cartão está bom; o problema é o leitor ou adaptador do PC |

![Proteção do cartão](img/protecao.png)

### Fazer backup (cartão para imagem)
Cria um arquivo `.img` com uma cópia exata, setor por setor, de todo o cartão. Serve para backup ou para clonar depois.
- Escolha o cartão e o local do arquivo `.img` (precisa ter espaço livre igual ao tamanho do cartão).
- Opção **Verificar a imagem depois de criar**: compara o arquivo com o cartão (demora o dobro).
- Setores ilegíveis são gravados como zeros e contados no resultado.
- Não salve a imagem dentro do próprio cartão (o app bloqueia).

### Gravar imagem (imagem para cartão)
Grava um arquivo `.img`, `.iso` ou `.bin` no cartão. **Apaga todo o conteúdo do cartão.**
- Escolha o cartão de destino e o arquivo de imagem (deve caber no cartão).
- Mantenha marcado **Verificar depois de gravar** (recomendado).
- Se o Windows pedir para formatar o disco ao final, clique em **Cancelar**.

### Clonar cartão
Clona direto de um cartão para outro. **Apaga todo o conteúdo do destino.**
- Escolha origem e destino. Normalmente o destino deve ser **igual ou maior** que a origem. Se for maior, o espaço extra fica sem partição.
- Dica: com um só leitor, use Fazer backup e depois Gravar imagem.

### Verificar setores
- **Leitura completa:** lê o cartão inteiro procurando setores ilegíveis e mostra a velocidade média. Não altera nada.
- **Comparar com imagem:** marque a opção e escolha um `.img` para ver se o cartão é idêntico a ele.
- **Teste de capacidade real (estilo H2testw):** grava dados em todo o cartão e confere se voltam idênticos. **APAGA o cartão** e pode levar horas. É o único teste que prova a capacidade verdadeira.

> A leitura completa não prova a capacidade. Um cartão falso lê sem erros. Para isso, use o teste de capacidade ou a **Autenticidade**.

### Recuperar e reparar
Escolha o que fazer na lista **"O que fazer com o cartão"**. Se não sabe o que está errado, comece pelo **1. Diagnosticar**.

**1. Diagnosticar o cartão** (não altera nada). Lê a tabela de partições (MBR/GPT), reconhece os sistemas de arquivos (FAT, exFAT, NTFS, ext2/3/4...), procura partições perdidas e explica no registro e na janela final **por que** o cartão está assim. Exemplos de resultado:

| Diagnóstico | O que significa | O que fazer |
|---|---|---|
| O próprio cartão informa só alguns MB (ex.: cartão de 128 GB aparecendo com 32 MB) | O controlador do cartão está devolvendo capacidade errada: cartão falso, controlador em modo de proteção ou leitor ruim. **Não é partição.** | Teste outro leitor/slot. Se continuar, não há reparo por software comum; troque o cartão. |
| As partições cobrem só uma parte do cartão | Uma imagem pequena foi gravada num cartão grande; o resto está sem partição. Os dados estão intactos. | Amplie a partição (GParted/MiniTool) ou use **Restaurar** (apaga tudo). |
| Partições Linux (ext4) | Normal em consoles (R36S, ArkOS, JELOS...). O Windows não lê e pede para formatar. | **Não formate.** Use **Ver / copiar arquivos**. |
| Tabela de partições apagada, partições encontradas | Cartão aparece como RAW; os dados estão lá. | **Reconstruir partições**. |
| Partição além do fim do cartão | Cartão falso ou imagem de cartão maior. | Rode **Autenticidade**. |
| Falhas de leitura | Defeito físico ou leitor ruim. | Salve o que puder e troque o cartão. |

**2. Ver / copiar arquivos** (só lê o cartão). Abre uma janela com as partições do cartão, inclusive as que o Windows **não** mostra (por exemplo ext4 de consoles Linux) e as encontradas fora da tabela. Navegue pelas pastas (dois cliques), selecione arquivos e copie para o PC ("Copiar selecionados" ou "Copiar tudo desta pasta"). Lê FAT12/16/32, exFAT e ext2/3/4. Nomes inválidos no Windows são ajustados e links simbólicos são ignorados.

**3. Reconstruir partições perdidas.** Reescreve o setor de partições (MBR) com as partições encontradas. **Não apaga dados.** Mostra a lista antes de gravar e guarda uma cópia do início do cartão em *Documentos* (`SDCLONETOOLS_backup_Disco...bin`). Funciona só em cartões MBR (não reconstrói GPT). Depois, remova e reinsira o cartão.

**4. Recuperar arquivos apagados** (só lê o cartão):
- Procura **JPG, PNG, PDF e vídeos MP4/MOV** apagados ou perdidos.
- Escolha uma pasta de destino em **outro disco**. Nunca salve no próprio cartão (o app bloqueia).
- Os arquivos saem com nomes numerados (`recuperado_000001.jpg`...), pois os nomes originais não existem mais.
- Se o cartão foi formatado ou regravado há muito tempo, os dados podem ter sido sobrescritos.
- Arquivos fragmentados e outros formatos (RAW de câmera, Word, ZIP...) não são recuperados.

**5. Restaurar o cartão** (**apaga tudo**):
- Limpa o cartão, cria uma partição do tamanho total e formata (nome `SDCARD`): **FAT32** até 32 GB, **exFAT** acima.
- Devolve o uso de um cartão com partições estranhas, protegido ou RAW. Não conserta cartão cujo controlador informa capacidade errada (o diagnóstico avisa).
- Cartões de consoles perdem o sistema: regrave a imagem depois (Imagem → Cartão).

> **Importante:** se o Windows pedir para formatar o cartão, clique em **Cancelar** e use o diagnóstico antes. Formatar pode apagar dados que ainda dá para salvar.

### Autenticidade (verdadeiro ou falso)
Ajuda a descobrir se o cartão é original ou falsificado.

1. Em **Marca escrita na etiqueta**, escolha SanDisk, Samsung, Kingston, Lexar, Sony, Transcend, ADATA, Silicon Power, Toshiba/Kioxia ou outra.
2. Em **classe de velocidade anunciada**, escolha a que está impressa no cartão (Class 10/U1, U3/V30, V60, V90).
3. Escolha entre:
   - **Teste rápido** (padrão, não destrutivo): lê o fabricante registrado no cartão (CID), compara com a etiqueta e mede a velocidade de leitura no início, meio e fim.
   - **Teste completo** (marque a caixa; **APAGA o cartão**): faz tudo isso e ainda prova a capacidade real, medindo também a velocidade de gravação.

**Como interpretar o resultado:**

| Resultado | Significado |
|---|---|
| Marca da etiqueta confere com o fabricante gravado | Bom sinal |
| Etiqueta SanDisk/Samsung, mas o cartão se identifica como outro | **Provável falsificação** |
| Fabricante "não listado" | Típico de cartão genérico ou clone |
| Capacidade real menor que a anunciada | **Cartão falso** (o app mostra a capacidade real estimada) |
| Velocidade de gravação abaixo da classe anunciada | Suspeito, ou cartão desgastado, ou leitor USB lento |

Depois do teste completo, use **Recuperar e reparar → Restaurar cartão** para formatar o cartão de novo.

## Limites importantes

- O fabricante do **chip de memória (NAND)** não fica gravado no cartão e nenhum software consegue lê-lo. O app identifica quem fabricou ou etiquetou o cartão (campo MID do CID), não o chip.
- O CID só é legível em **slot SD nativo** (notebook ou PC). Leitores USB escondem esses dados. Nesse caso o app avisa, e o teste completo de capacidade continua valendo.
- O nome mostrado na lista (ex.: "Generic STORAGE DEVICE") costuma ser do leitor, não do cartão.
- Marcas que usam controladores de terceiros (ex.: Kingston com Phison) geram só um **indício**, não uma conclusão.

## Segurança

- Operações que gravam sempre mostram o disco e pedem confirmação. **Confira o disco antes de confirmar.**
- A opção **Mostrar todos os discos** inclui HDs e SSDs externos. Use com muito cuidado.
- Não remova o cartão durante uma operação. Se fechar o app durante uma gravação, ele pede confirmação.

## Para desenvolvedores

| Arquivo | Conteúdo |
|---|---|
| `sdclone.c` | Interface Win32 e operações de disco |
| `usage.h` | Conteúdo do cartão: ocupação, tipos de arquivo e perfil de uso (sem Windows, testado em Linux) |
| `bench.h` | Julgamento do teste de desempenho (sem Windows) |
| `core.h` | Motor de recuperação (assinaturas de arquivo), padrão do teste de capacidade e tabela de fabricantes |
| `fsread.h` | Leitores somente leitura de FAT12/16/32, exFAT e ext2/3/4 (independentes de Windows) |
| `diag.h` | Diagnóstico: MBR/GPT, partições perdidas, veredito e reconstrução do MBR (independente de Windows) |
| `repair.h` | Parte do app que usa o diagnóstico: relatório, reconstrução e navegador de arquivos |
| `fstest.c`, `tests/run_tests.py` | Ferramenta e testes automatizados dos leitores e do diagnóstico (Linux) |
| `test_core.c` | Teste do motor de recuperação (roda em Linux) |
| `sdclone.rc`, `sdclone.manifest`, `sdclone.ico`, `mkicon.py` | Recursos: ícone e manifesto (exige administrador) |
| `build.sh` | Compila os dois `.exe` |

**Compilar** (precisa de Python e `pip install ziglang`; funciona a partir do Linux, macOS ou Windows):

```sh
./build.sh
```

**Testar o motor de recuperação** (Linux):

```sh
gcc -O1 -o test_core test_core.c && ./test_core
```

**Testar leitores de sistemas de arquivos e diagnóstico** (Linux, como root; precisa de `dosfstools mtools exfatprogs exfat-fuse e2fsprogs fdisk`):

```sh
python3 tests/run_tests.py
```

Cria imagens reais (FAT12/16/32, exFAT, ext2/3/4, MBR, GPT, tabela perdida, cartão minúsculo, etc.), lê todos os arquivos e confere o CRC.

C puro com a API Win32, sem dependências além das DLLs do próprio Windows.

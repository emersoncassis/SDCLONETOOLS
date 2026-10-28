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
2. Clique na função desejada nos botões do topo.
3. Escolha o cartão na lista. Se ele não aparecer, clique em **Atualizar**.
4. Preencha o que a função pedir (arquivo de imagem, pasta etc.).
5. Clique em **Iniciar** e confirme o aviso. Toda operação que apaga dados mostra o disco escolhido e pede confirmação.
6. Acompanhe a barra de progresso, a velocidade e o tempo restante. **Cancelar** interrompe a operação.
7. Ao final aparece uma mensagem com o resultado. O registro completo fica na caixa de texto no rodapé.

## As funções

### Cartão → Imagem
Cria um arquivo `.img` com uma cópia exata, setor por setor, de todo o cartão. Serve para backup ou para clonar depois.
- Escolha o cartão e o local do arquivo `.img` (precisa ter espaço livre igual ao tamanho do cartão).
- Opção **Verificar a imagem depois de criar**: compara o arquivo com o cartão (demora o dobro).
- Setores ilegíveis são gravados como zeros e contados no resultado.
- Não salve a imagem dentro do próprio cartão (o app bloqueia).

### Imagem → Cartão
Grava um arquivo `.img`, `.iso` ou `.bin` no cartão. **Apaga todo o conteúdo do cartão.**
- Escolha o cartão de destino e o arquivo de imagem (deve caber no cartão).
- Mantenha marcado **Verificar depois de gravar** (recomendado).
- Se o Windows pedir para formatar o disco ao final, clique em **Cancelar**.

### Cartão → Cartão
Clona direto de um cartão para outro. **Apaga todo o conteúdo do destino.**
- Escolha origem e destino. Normalmente o destino deve ser **igual ou maior** que a origem. Se for maior, o espaço extra fica sem partição.
- **Copiar só os dados (destino menor):** marque a opção quando o destino for menor mas os dados caibam nele. O app analisa a origem e escolhe sozinho:
  - se as **partições** ocupam menos que o destino (ex.: imagem de 8 GB em cartão de 64 GB), copia exatamente só essa área (cópia fiel, serve para sistemas de consoles);
  - se a partição é **maior que o destino** mas tem uma única partição FAT/exFAT, formata o destino e copia os **arquivos** (datas e atributos não são preservados; não serve para cartão bootável);
  - cartões com ext4/Linux ou GPT que não cabem no destino não podem ser reduzidos com segurança; o app explica e não altera nada.
- Dica: com um só leitor, use Cartão → Imagem e depois Imagem → Cartão.

### Verificar cartão
- **Leitura completa:** lê o cartão inteiro procurando setores ilegíveis e mostra a velocidade média. Não altera nada.
- **Comparar com imagem:** marque a opção e escolha um `.img` para ver se o cartão é idêntico a ele.
- **Teste de capacidade real (estilo H2testw):** grava dados em todo o cartão e confere se voltam idênticos. **APAGA o cartão** e pode levar horas. É o único teste que prova a capacidade verdadeira.

> A leitura completa não prova a capacidade. Um cartão falso lê sem erros. Para isso, use o teste de capacidade ou a **Autenticidade**.

### Recuperar cartão (reparar cartão sem acesso ou com capacidade errada)
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

Depois do teste completo, use **Recuperar cartão → 5. Restaurar** para formatar o cartão de novo.

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

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
- Escolha origem e destino. O destino deve ser **igual ou maior** que a origem. Se for maior, o espaço extra fica sem partição.
- Dica: com um só leitor, use Cartão → Imagem e depois Imagem → Cartão.

### Verificar cartão
- **Leitura completa:** lê o cartão inteiro procurando setores ilegíveis e mostra a velocidade média. Não altera nada.
- **Comparar com imagem:** marque a opção e escolha um `.img` para ver se o cartão é idêntico a ele.
- **Teste de capacidade real (estilo H2testw):** grava dados em todo o cartão e confere se voltam idênticos. **APAGA o cartão** e pode levar horas. É o único teste que prova a capacidade verdadeira.

> A leitura completa não prova a capacidade. Um cartão falso lê sem erros. Para isso, use o teste de capacidade ou a **Autenticidade**.

### Recuperar cartão
Dois modos:

**Recuperar arquivos apagados** (só lê o cartão):
- Procura **JPG, PNG, PDF e vídeos MP4/MOV** apagados ou perdidos.
- Escolha uma pasta de destino em **outro disco**. Nunca salve no próprio cartão (o app bloqueia).
- Os arquivos saem com nomes numerados (`recuperado_000001.jpg`...), pois os nomes originais não existem mais.
- Se o cartão foi formatado ou regravado há muito tempo, os dados podem ter sido sobrescritos.
- Arquivos fragmentados e outros formatos (RAW de câmera, Word, ZIP...) não são recuperados.

**Restaurar o cartão** (**apaga tudo**):
- Limpa o cartão, cria uma partição e formata (nome `SDCARD`): **FAT32** até 32 GB, **exFAT** acima.
- Corrige cartão que aparece como **RAW**, protegido ou com capacidade errada no Windows.

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

Depois do teste completo, use **Recuperar cartão → Restaurar** para formatar o cartão de novo.

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

C puro com a API Win32, sem dependências além das DLLs do próprio Windows.

# Como contribuir com o SDCLONETOOLS

Obrigado por ajudar! Este projeto é feito pela e para a comunidade que lida com cartões SD em gamesticks e consoles portáteis.

## O que ajuda mais

1. **Relatos de teste.** Abra uma issue contando em qual cartão e leitor você testou, qual função usou e o resultado. Cartões falsos reais (marca na etiqueta, o que o app mostrou, capacidade real) são ouro.
2. **Novos fabricantes.** A tabela de IDs (campo MID do CID) está em `core.h` (`MID_TABLE`). Para acrescentar um, informe o MID, o nome e como você confirmou (ex.: cartão original de uma marca, lido em slot SD nativo).
3. **Correções e melhorias** no código, na interface ou na documentação.
4. **Tradução** do app e dos guias.

## Antes de abrir um pull request

- Mantenha o código em **C puro com a API Win32**, sem novas dependências.
- Rode o teste do motor de recuperação:
  ```sh
  gcc -O1 -o test_core test_core.c && ./test_core
  ```
  Ele deve terminar com `RESULTADO: TUDO OK`. Se mexer em `core.h`, acrescente um caso em `test_core.c`.
- Se mexer em `fsread.h` ou `diag.h`, rode também (Linux, como root; veja os pacotes necessários no README):
  ```sh
  sudo python3 tests/run_tests.py
  ```
  Acrescente um caso em `tests/run_tests.py` para cada comportamento novo.
- Compile com `./build.sh` e confirme que não há erros.
- **Teste em cartão sem dados importantes**, especialmente qualquer mudança que grave no disco.
- Mudanças em operações que gravam devem manter as proteções: nunca listar o disco do sistema e sempre pedir confirmação.
- Um pull request por assunto, com uma descrição curta do que mudou e como foi testado.

## Relatando bugs

Use o modelo de issue e inclua: versão do Windows, modelo do cartão e do leitor (slot nativo ou USB), função usada e o conteúdo da caixa de registro do app.

## Segurança

Se encontrar uma falha que possa causar perda de dados em disco que não seja o escolhido, descreva-a em uma issue marcada como **segurança** (sem publicar detalhes que facilitem uso indevido).

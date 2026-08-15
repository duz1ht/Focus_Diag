# Focus Diagnostic DLL

DLL x86 de diagnóstico passivo para jogos DirectX 8. Ela registra o ciclo de
perda e recuperação de foco sem forçar `SetFocus`, `Reset`, `Acquire` ou estado
do cursor.

## Compilar no Visual Studio 2022

1. Instale a carga de trabalho **Desenvolvimento para desktop com C++** e o
   Windows 10/11 SDK.
2. Abra `FocusDiagnostic.sln`.
3. Selecione **Release** e **x86**. Não use x64 para um jogo de 32 bits.
4. Use **Build > Build Solution**.

O resultado será criado em `bin\Release\FocusDiagnostic.dll`. O projeto inclui
as declarações mínimas da ABI do DirectX 8, portanto não requer a instalação do
antigo DirectX SDK nem bibliotecas externas de hooking.

## Carregar no jogo

Copiar a DLL para a pasta do jogo não a carrega automaticamente. Use o mesmo
método de injeção/carregamento de DLL já adotado para o jogo e carregue a DLL
cedo, antes da criação dos objetos Direct3D e DirectInput. O carregador precisa
ter a mesma arquitetura do processo (x86).

> Faça testes apenas em uma cópia local/offline do jogo. Injetores e DLLs podem
> ser sinalizados por sistemas anticheat. Este projeto não inclui um injetor.

Quando carregada, a DLL cria `FocusDiagnostic.log` ao lado dela. Se o processo
não tiver permissão de escrita nessa pasta, execute a partir de uma pasta
gravável ou ajuste as permissões.

## Procedimento de teste

1. Entre no gameplay e pressione **F10** para gravar `USER MARKER`.
2. Faça `Alt+Tab`, aguarde e retorne ao jogo.
3. Se houver falha, pressione **F11** para gravar um snapshot.
4. Feche normalmente o jogo e abra `FocusDiagnostic.log`.

O logger registra:

- ativação, foco, redimensionamento e mudança de display da janela;
- `TestCooperativeLevel`, `Reset` e os primeiros `Present` da recuperação;
- `Acquire` e `Unacquire` de mouse e teclado;
- `ClipCursor` e `SetCursorPos` quando importados diretamente pelo executável;
- contador de frames, HRESULTs legíveis e IDs das threads;
- `LIKELY FAILURE AREA` no snapshot e um resumo após timeout de cinco segundos.

Categorias possíveis incluem `WINDOW_ACTIVATION`, `D3D_DEVICE_LOST`,
`D3D_RESET`, `RENDER_LOOP`, `KEYBOARD_ACQUIRE`, `MOUSE_ACQUIRE` e
`INCONCLUSIVE`.

## Como os hooks funcionam

- As funções Win32 e as fábricas `Direct3DCreate8`/`DirectInput8Create` são
  interceptadas na IAT do executável principal.
- Os objetos COM capturados recebem uma cópia de sua vtable com somente os
  métodos observados substituídos.
- Os eventos são enfileirados e uma thread separada grava o arquivo para reduzir
  interferência no timing do jogo.
- A inicialização aguarda por até 30 segundos pela janela principal do processo.

## Limitações da versão 0.1

- A DLL deve ser carregada antes de `Direct3DCreate8` e `DirectInput8Create`.
- APIs resolvidas exclusivamente por `GetProcAddress`, chamadas por módulos
  auxiliares ou objetos criados antes do carregamento não são interceptados.
- Esta versão acompanha um objeto Direct3D e um dispositivo de cada tipo
  (mouse/teclado), que é o padrão esperado para o jogo alvo.
- O resultado `INCONCLUSIVE` significa que os eventos observados não bastam para
  atribuir a falha com segurança; ele não deve ser tratado como confirmação de
  que todos os subsistemas estão corretos.

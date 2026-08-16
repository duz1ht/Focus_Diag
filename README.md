# Focus Diagnostic — proxy dinput8.dll

Proxy DLL x86 de diagnóstico passivo para jogos DirectX 8. Ela registra o ciclo de
perda e recuperação de foco sem forçar `SetFocus`, `Reset`, `Acquire` ou estado
do cursor.

## Compilar no Visual Studio 2022

1. Instale a carga de trabalho **Desenvolvimento para desktop com C++** e o
   Windows 10/11 SDK.
2. Abra `FocusDiagnostic.sln`.
3. Selecione **Release** e **x86**. Não use x64 para um jogo de 32 bits.
4. Use **Build > Build Solution**.

O resultado será criado em `bin\Release\dinput8.dll`. O projeto inclui
as declarações mínimas da ABI do DirectX 8, portanto não requer a instalação do
antigo DirectX SDK nem bibliotecas externas de hooking.

## Instalar no jogo

Copie `bin\Release\dinput8.dll` e `bin\Release\FocusDiagnostic.ini` para a mesma
pasta do executável do jogo. Quando
o jogo importar `DirectInput8Create`, o Windows carregará automaticamente a
proxy, que encaminhará as chamadas para a `dinput8.dll` original do diretório do
sistema. Não é necessário usar um injetor. O jogo precisa ser x86.

> Faça testes apenas em uma cópia local/offline do jogo. DLLs proxy podem ser
> sinalizadas por sistemas anticheat.

Quando carregada, a DLL cria `FocusDiagnostic.log` ao lado dela. Se o processo
não tiver permissão de escrita nessa pasta, execute a partir de uma pasta
gravável ou ajuste as permissões.

## Procedimento de teste

Por segurança, o arquivo fornecido começa em modo **proxy-only**, com todos os
hooks invasivos desligados. Primeiro execute o jogo dessa forma e confirme que a
janela abre normalmente. O log deve conter `dinput8=PROXY-ONLY`.

Depois, feche o jogo e habilite apenas um subsistema por vez em
`FocusDiagnostic.ini`, nesta ordem recomendada:

```ini
[Hooks]
Window=1
D3D8=0
D3D8CreateDevice=0
D3D8Device=0
DirectInput=0
Cursor=0
```

Teste novamente, depois habilite `DirectInput` e `Cursor`. Para Direct3D, use
três etapas separadas: primeiro `D3D8=1`, depois `D3D8CreateDevice=1` e somente
por último `D3D8Device=1`. Se o jogo voltar a fechar ao habilitar uma opção, deixe-a em `0` e
guarde o último `FocusDiagnostic.log`; isso identifica o hook incompatível.

`D3D8=1` intercepta apenas a função de fábrica e não altera objetos COM.
`D3D8CreateDevice=1` adiciona a observação de `IDirect3D8::CreateDevice`.
`D3D8Device=1` habilita `TestCooperativeLevel`, `Reset` e `Present`. Não ative
uma etapa posterior sem manter as anteriores habilitadas.

Com `Window=1`, o procedimento de foco é:

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

- A proxy exporta as cinco entradas convencionais de `dinput8.dll`, carrega a
  biblioteca verdadeira diretamente do diretório do sistema e encaminha as chamadas.
- As funções Win32 e a fábrica `Direct3DCreate8` são interceptadas na IAT do
  executável principal; `DirectInput8Create` é capturada diretamente pela proxy.
- Os objetos COM capturados recebem uma cópia de sua vtable com somente os
  métodos observados substituídos.
- Os eventos são enfileirados e uma thread separada grava o arquivo para reduzir
  interferência no timing do jogo.
- A inicialização aguarda por até 30 segundos pela janela principal do processo.

## Limitações da versão 0.1

- O executável precisa importar `dinput8.dll`; confirme com `dumpbin /imports`.
- Se já existir uma `dinput8.dll` na pasta do jogo, não a sobrescreva sem antes
  identificar se ela pertence a outro mod ou wrapper.
- APIs de D3D/cursor resolvidas exclusivamente por `GetProcAddress` ou chamadas
  por módulos auxiliares não são interceptadas.
- Esta versão acompanha um objeto Direct3D e um dispositivo de cada tipo
  (mouse/teclado), que é o padrão esperado para o jogo alvo.
- O resultado `INCONCLUSIVE` significa que os eventos observados não bastam para
  atribuir a falha com segurança; ele não deve ser tratado como confirmação de
  que todos os subsistemas estão corretos.
- Quando `D3D8Device=0`, o contador de frames permanece zero por falta de
  instrumentação. Nesse modo o diagnóstico não classifica mais a ausência de
  `Present` como `RENDER_LOOP`.

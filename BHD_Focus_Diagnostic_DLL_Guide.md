# Guia de criação de uma DLL de diagnóstico de foco para DirectX 8

## 1. Objetivo da DLL

Esta DLL deve ser criada **exclusivamente como ferramenta de diagnóstico** para investigar problemas de perda e recuperação de foco em um jogo antigo baseado em **DirectX 8**, especialmente durante operações como:

- `Alt+Tab` para sair do jogo;
- retorno ao jogo depois de usar o desktop;
- minimização e restauração da janela;
- troca entre fullscreen e desktop;
- perda temporária de mouse ou teclado;
- tela preta, congelamento ou falha de renderização depois de recuperar o foco.

A intenção **não é corrigir o problema imediatamente**.

A intenção inicial é descobrir, com precisão, **em qual etapa do processo de recuperação ocorre a falha**.

A DLL deve observar e registrar o comportamento do jogo sem modificar sua lógica além do estritamente necessário para instrumentação.

---

## 2. Problema que queremos investigar

Em um jogo DirectX 8, recuperar o foco depois de um `Alt+Tab` pode envolver várias etapas independentes.

Uma falha aparente de "foco" pode, na verdade, acontecer em diferentes pontos:

```text
Windows devolveu o foco
        ↓
mas o jogo ainda considera o Direct3D device perdido
```

ou:

```text
Direct3D device foi restaurado
        ↓
mas algum recurso gráfico não foi recriado corretamente
```

ou:

```text
Renderização voltou
        ↓
mas mouse ou teclado não foram readquiridos
```

ou:

```text
A janela recebeu foco
        ↓
mas alguma rotina antiga de ativação do jogo não respondeu
como esperado
```

ou ainda:

```text
Tudo voltou aparentemente ao normal
        ↓
mas o cursor, clipping ou posição do mouse ficaram incorretos
```

Portanto, não devemos partir diretamente para uma correção.

Primeiro precisamos observar o ciclo completo e identificar **o primeiro ponto onde um Alt+Tab que falha se comporta de forma diferente de um Alt+Tab que funciona**.

---

# 3. Estratégia recomendada

A primeira versão da DLL deve usar **logging em arquivo**.

Um overlay pode ser adicionado depois, mas não deve ser o primeiro método de diagnóstico.

## Por que começar por log?

Um overlay pode:

- interferir no rendering;
- criar hooks adicionais no Direct3D;
- modificar estados do device;
- alterar timing;
- influenciar fullscreen;
- alterar o comportamento do problema que estamos tentando observar.

Por isso, a primeira versão deve ser o mais passiva possível.

Fluxo recomendado:

```text
DLL de diagnóstico
        ↓
hooks somente para observação
        ↓
log com timestamps
        ↓
reproduzir Alt+Tab
        ↓
comparar tentativa bem-sucedida com tentativa problemática
        ↓
encontrar a primeira divergência
```

---

# 4. Princípio mais importante

A DLL deve tentar **observar antes de alterar**.

Sempre que possível:

1. interceptar a chamada;
2. registrar parâmetros;
3. chamar a função original;
4. registrar o resultado;
5. devolver exatamente o mesmo resultado ao jogo.

Exemplo conceitual:

```cpp
HRESULT HookedAcquire(...)
{
    Log("Mouse Acquire called");

    HRESULT result = OriginalAcquire(...);

    Log("Mouse Acquire returned: 0x%08X", result);

    return result;
}
```

A primeira versão não deve:

- forçar foco;
- chamar `Reset()` por conta própria;
- chamar `Acquire()` adicional;
- forçar `ClipCursor`;
- alterar resolução;
- modificar janela;
- alterar o fluxo normal do jogo;
- tentar corrigir automaticamente o problema.

Essas ações podem ser testadas posteriormente, depois que a causa estiver identificada.

---

# 5. Arquitetura sugerida

Para um jogo dessa época, a DLL provavelmente deverá ser compilada como **32-bit / x86**, caso o executável do jogo também seja 32-bit.

Estrutura sugerida:

```text
FocusDiagnostic.dll
│
├── dllmain.cpp
├── logger.cpp
├── logger.h
│
├── window_hooks.cpp
├── window_hooks.h
│
├── d3d8_hooks.cpp
├── d3d8_hooks.h
│
├── dinput_hooks.cpp
├── dinput_hooks.h
│
├── cursor_hooks.cpp
├── cursor_hooks.h
│
└── diagnostics.cpp
```

Não é obrigatório separar dessa forma, mas ajuda a manter cada subsistema isolado.

---

# 6. Sistema de log

O log é a parte mais importante da primeira versão.

Arquivo sugerido:

```text
FocusDiagnostic.log
```

Exemplo:

```text
[20:31:42.103] [WINDOW] WM_ACTIVATEAPP FALSE
[20:31:42.104] [WINDOW] WM_KILLFOCUS
[20:31:42.107] [DINPUT] Mouse Unacquire()
[20:31:42.111] [D3D8] TestCooperativeLevel() -> D3DERR_DEVICELOST

[20:31:45.521] [WINDOW] WM_ACTIVATEAPP TRUE
[20:31:45.522] [WINDOW] WM_SETFOCUS
[20:31:45.525] [D3D8] TestCooperativeLevel() -> D3DERR_DEVICENOTRESET
[20:31:45.527] [D3D8] Reset() called
[20:31:45.581] [D3D8] Reset() -> D3D_OK
[20:31:45.584] [DINPUT] Mouse Acquire() -> DI_OK
[20:31:45.585] [CURSOR] ClipCursor(0, 0, 1920, 1080)
[20:31:45.590] [D3D8] Present() -> D3D_OK
```

---

# 7. Timestamp

Cada evento deve receber timestamp.

Idealmente usar:

```cpp
QueryPerformanceCounter()
```

e:

```cpp
QueryPerformanceFrequency()
```

ou outro relógio monotônico de alta resolução.

O objetivo é conseguir determinar a ordem real dos eventos.

Exemplo:

```text
WM_SETFOCUS
    ↓ 2 ms

TestCooperativeLevel
    ↓ 1 ms

Reset
    ↓ 46 ms

Mouse Acquire
    ↓ 2 ms

ClipCursor
    ↓ 3 ms

Present
```

A ordem pode ser tão importante quanto o resultado das chamadas.

---

# 8. Identificação da thread

Também é recomendado registrar:

```cpp
GetCurrentThreadId()
```

Exemplo:

```text
[20:31:45.522] [TID 4284] [WINDOW] WM_SETFOCUS
```

Isso pode mostrar se o gerenciamento de janela, input e rendering está acontecendo em threads diferentes.

---

# 9. Eventos de janela que devem ser monitorados

A DLL deve registrar pelo menos:

```text
WM_ACTIVATE
WM_ACTIVATEAPP
WM_SETFOCUS
WM_KILLFOCUS
WM_SIZE
WM_SYSCOMMAND
WM_DISPLAYCHANGE
```

Também pode ser útil observar:

```text
WM_ENTERSIZEMOVE
WM_EXITSIZEMOVE
WM_WINDOWPOSCHANGED
```

Para `WM_ACTIVATE`, registrar o estado:

```text
WA_INACTIVE
WA_ACTIVE
WA_CLICKACTIVE
```

Para `WM_SIZE`, registrar:

```text
SIZE_MINIMIZED
SIZE_MAXIMIZED
SIZE_RESTORED
```

Exemplo:

```text
[WINDOW] WM_ACTIVATE -> WA_INACTIVE
[WINDOW] WM_SIZE -> SIZE_MINIMIZED

...

[WINDOW] WM_ACTIVATE -> WA_ACTIVE
[WINDOW] WM_SIZE -> SIZE_RESTORED
```

---

# 10. Como interceptar mensagens da janela

Uma possibilidade é obter o `HWND` principal do jogo e substituir temporariamente seu `WndProc` usando:

```cpp
SetWindowLongPtr()
```

com:

```text
GWLP_WNDPROC
```

A função original deve ser preservada.

Fluxo:

```text
Mensagem chega
    ↓
HookedWndProc
    ↓
registra mensagem
    ↓
chama WndProc original
    ↓
retorna resultado original
```

É importante evitar alterar o comportamento normal da janela na primeira versão.

---

# 11. Direct3D 8

Esta é uma das áreas mais importantes.

Devem ser monitoradas principalmente:

```cpp
IDirect3DDevice8::TestCooperativeLevel()
IDirect3DDevice8::Reset()
IDirect3DDevice8::Present()
```

Também pode ser útil monitorar:

```cpp
IDirect3D8::CreateDevice()
```

para capturar o ponteiro inicial do `IDirect3DDevice8`.

---

# 12. TestCooperativeLevel

Registrar todas as chamadas de:

```cpp
IDirect3DDevice8::TestCooperativeLevel()
```

e seus resultados.

Resultados particularmente importantes:

```text
D3D_OK
D3DERR_DEVICELOST
D3DERR_DEVICENOTRESET
```

Exemplo de comportamento saudável:

```text
TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICENOTRESET
Reset -> D3D_OK
TestCooperativeLevel -> D3D_OK
```

Exemplo suspeito:

```text
WM_SETFOCUS

TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICELOST
...
```

Nesse caso o Windows devolveu foco, mas o Direct3D não conseguiu avançar para um estado em que o device pudesse ser resetado.

---

# 13. Reset

Interceptar:

```cpp
IDirect3DDevice8::Reset()
```

Registrar:

- timestamp;
- thread;
- chamada;
- resultado;
- resolução solicitada;
- formato;
- modo windowed/fullscreen;
- backbuffer width;
- backbuffer height;
- refresh rate, se disponível;
- `hDeviceWindow`, quando aplicável.

Exemplo:

```text
[D3D8] Reset called
[D3D8] BackBuffer = 1920x1080
[D3D8] Windowed = FALSE
[D3D8] Reset -> D3D_OK
```

Se o Reset falhar:

```text
[D3D8] Reset -> 0x88760868
```

Também registrar uma versão traduzida do erro, quando possível.

---

# 14. Present

Monitorar:

```cpp
IDirect3DDevice8::Present()
```

Não é necessário escrever uma linha para todos os frames durante uma sessão inteira, porque isso pode gerar um log enorme.

Uma estratégia melhor:

- contar frames;
- armazenar o timestamp do último `Present`;
- registrar mudança de estado;
- registrar erros;
- registrar os primeiros `Present()` depois de recuperar foco.

Exemplo:

```text
[D3D8] Present resumed after 3421 ms
[D3D8] Present -> D3D_OK
[D3D8] Frame counter = 184254
```

---

# 15. Contador de frames

Manter:

```cpp
uint64_t frameCounter;
```

Incrementado em cada `Present()` bem-sucedido.

Isso ajuda a responder:

> O jogo realmente voltou a renderizar depois de recuperar o foco?

Exemplo:

```text
FRAME 184251 Present OK
FRAME 184252 Present OK

WM_ACTIVATEAPP FALSE

...

WM_ACTIVATEAPP TRUE

FRAME 184253 Present OK
FRAME 184254 Present OK
```

Se o contador parar completamente, o problema pode estar na renderização ou no game loop.

---

# 16. DirectInput

O jogo pode recuperar Direct3D corretamente e ainda falhar no input.

Monitorar, quando possível:

```cpp
IDirectInputDevice8::Acquire()
IDirectInputDevice8::Unacquire()
IDirectInputDevice8::GetDeviceState()
IDirectInputDevice8::GetDeviceData()
```

Registrar separadamente mouse e teclado.

Exemplo:

```text
[DINPUT][MOUSE] Unacquire -> DI_OK
[DINPUT][KEYBOARD] Unacquire -> DI_OK

...

[DINPUT][KEYBOARD] Acquire -> DI_OK
[DINPUT][MOUSE] Acquire -> DIERR_OTHERAPPHASPRIO
```

Nesse exemplo, o problema que parece ser de foco pode na realidade estar no reacquire do mouse.

---

# 17. Identificar mouse e teclado

Quando os dispositivos DirectInput forem criados, armazenar seus ponteiros separadamente.

Estado interno sugerido:

```text
MouseDevice
KeyboardDevice
```

Assim o log pode mostrar:

```text
[DINPUT][MOUSE]
```

em vez de apenas:

```text
[DINPUT][DEVICE 0x12345678]
```

---

# 18. Cursor e clipping

Esta parte é especialmente importante em jogos antigos que controlam manualmente o cursor.

Monitorar:

```cpp
ClipCursor()
GetClipCursor()
SetCursorPos()
GetCursorPos()
ShowCursor()
SetCapture()
ReleaseCapture()
```

Também pode ser útil observar:

```cpp
GetClientRect()
GetWindowRect()
ClientToScreen()
ScreenToClient()
```

---

# 19. ClipCursor

Cada chamada deve registrar o retângulo recebido.

Exemplo:

```text
[CURSOR] ClipCursor
         Left   = 0
         Top    = 0
         Right  = 1920
         Bottom = 1080
```

Ou:

```text
[CURSOR] ClipCursor(NULL)
```

Também pode ser útil chamar `GetClipCursor()` depois da função original para confirmar o estado real aplicado pelo Windows.

Exemplo:

```text
[CURSOR] ClipCursor requested: 0,0,1920,1080
[CURSOR] Actual clip rect:     0,0,1920,1080
```

---

# 20. SetCursorPos

Registrar:

```text
X
Y
```

Exemplo:

```text
[CURSOR] SetCursorPos(960, 540)
```

Isso pode revelar reposicionamentos inesperados imediatamente depois do Alt+Tab.

---

# 21. Estado da janela

Quando houver perda ou recuperação de foco, registrar um snapshot contendo:

```text
HWND
GetForegroundWindow()
GetActiveWindow()
GetFocus()
IsIconic()
IsWindowVisible()
GetWindowRect()
GetClientRect()
```

Exemplo:

```text
===== WINDOW SNAPSHOT =====

Game HWND:          0x000307AC
Foreground Window:  0x000307AC
Active Window:      0x000307AC
Focused Window:     0x000307AC
Minimized:          NO
Visible:            YES

Window Rect:
0,0,1920,1080

Client Rect:
0,0,1920,1080
```

---

# 22. Snapshot completo de diagnóstico

A DLL deve possuir uma função capaz de registrar o estado atual completo.

Exemplo:

```text
========== DIAGNOSTIC SNAPSHOT ==========

Time:              20:31:45.590
Frame:             184254

WINDOW
Foreground:        YES
Active:            YES
Focused:           YES
Minimized:         NO

DIRECT3D 8
Device:            VALID
Cooperative Level: D3D_OK
Last Reset:        D3D_OK
Last Present:      3 ms ago

DIRECTINPUT
Mouse:             ACQUIRED
Keyboard:          ACQUIRED

CURSOR
Clip Active:       YES
Clip Rect:         0,0,1920,1080
Cursor Position:   960,540

=========================================
```

---

# 23. Tecla para snapshot manual

Pode ser útil adicionar uma tecla de diagnóstico, por exemplo:

```text
F11
```

Ao pressioná-la, a DLL escreve no log:

```text
========== MANUAL SNAPSHOT ==========
...
```

Isto permite que o tester pressione a tecla exatamente quando perceber que o jogo retornou de forma incorreta.

Se `F11` já for usada pelo jogo, escolher outra tecla.

---

# 24. Marcadores manuais

Também pode ser útil uma tecla que apenas adicione:

```text
========== USER MARKER ==========
```

ao log.

Por exemplo:

```text
F10
```

O tester pode:

1. entrar no jogo;
2. pressionar F10;
3. fazer Alt+Tab;
4. retornar;
5. pressionar F10 novamente.

O log fica fácil de analisar.

---

# 25. Hooking

Existem várias formas de implementar os hooks.

Possibilidades:

## WinAPI

Para funções como:

```text
ClipCursor
SetCursorPos
ShowCursor
SetCapture
ReleaseCapture
```

pode-se usar:

- IAT hooking;
- inline hooking;
- biblioteca de hooking.

## COM / Direct3D 8

Para:

```text
IDirect3DDevice8
IDirectInputDevice8
```

pode-se interceptar métodos através da vtable do objeto COM.

Outra possibilidade é interceptar a criação do objeto e substituir os ponteiros relevantes da vtable.

---

# 26. Biblioteca de hooking

Uma biblioteca pequena como **MinHook** pode simplificar hooks de funções normais.

Entretanto, para métodos COM de Direct3D e DirectInput, ainda pode ser necessário trabalhar com:

- ponteiro do objeto;
- vtable;
- endereço real do método.

É importante documentar claramente cada hook.

Exemplo:

```text
Hook: IDirect3DDevice8::Reset
Original address: 0xXXXXXXXX
Hook address:     0xXXXXXXXX
Installed:        YES
```

---

# 27. Evitar hooks desnecessários

Não começar interceptando dezenas de APIs.

Primeira versão recomendada:

## Window

```text
WM_ACTIVATE
WM_ACTIVATEAPP
WM_SETFOCUS
WM_KILLFOCUS
WM_SIZE
```

## D3D8

```text
TestCooperativeLevel
Reset
Present
```

## DirectInput

```text
Acquire
Unacquire
```

## Cursor

```text
ClipCursor
SetCursorPos
```

Depois expandir somente se os logs não forem suficientes.

---

# 28. Performance

Logging excessivo pode alterar timing.

Portanto:

- evitar `FlushFileBuffers()` após cada linha;
- usar buffer;
- evitar escrever todos os frames;
- evitar operações pesadas dentro de hooks;
- não usar console se isso alterar comportamento de foco;
- não fazer alocação complexa em cada chamada.

Uma opção é armazenar eventos em uma fila e usar uma thread dedicada para gravar o arquivo.

---

# 29. Thread de logging

Arquitetura recomendada:

```text
Hook
  ↓
cria evento pequeno
  ↓
coloca em fila
  ↓
retorna imediatamente ao jogo

Thread de logging
  ↓
lê fila
  ↓
formata texto
  ↓
escreve no arquivo
```

Isso reduz a interferência no timing do jogo.

---

# 30. Segurança contra recursão

Alguns hooks podem chamar APIs que também estão sendo monitoradas.

É necessário evitar recursão.

Exemplo:

```cpp
thread_local bool insideHook = false;
```

Fluxo conceitual:

```cpp
if (insideHook)
    return OriginalFunction(...);

insideHook = true;

Log(...);
auto result = OriginalFunction(...);
Log(...);

insideHook = false;

return result;
```

---

# 31. Inicialização da DLL

Evitar fazer trabalho complexo diretamente em:

```cpp
DllMain()
```

Especialmente porque o Windows mantém o loader lock durante determinadas chamadas.

Estratégia:

```cpp
DLL_PROCESS_ATTACH
    ↓
DisableThreadLibraryCalls()
    ↓
CreateThread()
    ↓
InitializationThread()
```

A thread de inicialização pode então:

1. abrir o logger;
2. localizar a janela;
3. instalar hooks;
4. localizar Direct3D;
5. localizar DirectInput;
6. iniciar diagnóstico.

---

# 32. Encerramento

Ao descarregar a DLL:

1. desabilitar hooks;
2. restaurar `WndProc`;
3. restaurar vtables;
4. aguardar thread de log;
5. gravar eventos restantes;
6. fechar arquivo.

Registrar:

```text
[DIAGNOSTIC] DLL shutting down
[DIAGNOSTIC] Hooks removed
[DIAGNOSTIC] Log closed
```

---

# 33. Cabeçalho do arquivo de log

Todo log deve começar com informações do ambiente.

Exemplo:

```text
========================================
FOCUS DIAGNOSTIC
========================================

DLL Version:       0.1
Process:           dfbhd.exe
Process ID:        4820
Architecture:      x86
OS:                Windows 11
Start Time:        20:27:14

Game Window:
HWND:              0x000307AC
Resolution:        1920x1080

========================================
```

Se possível, incluir também:

```text
Executable version
Executable hash
Loaded d3d8.dll path
Loaded dinput8.dll path
```

Isso ajuda a detectar diferenças entre máquinas e wrappers.

---

# 34. Detectar wrappers

É útil registrar de onde foram carregadas:

```text
d3d8.dll
dinput8.dll
```

Exemplo:

```text
D3D8 Module:
C:\Game\d3d8.dll
```

versus:

```text
C:\Windows\System32\d3d8.dll
```

Isso ajuda a identificar se dgVoodoo, WineD3D ou outro wrapper está presente.

A DLL de diagnóstico não deve assumir que o jogo está usando a implementação original do sistema.

---

# 35. HRESULT legível

Além do valor hexadecimal, tentar registrar um nome legível.

Em vez de:

```text
0x88760869
```

preferir:

```text
0x88760869 (D3DERR_DEVICELOST)
```

Isso acelera enormemente a análise.

---

# 36. Sessões de teste

Cada teste deve ser marcado.

Exemplo:

```text
========== TEST 01 START ==========
Scenario: Alt+Tab once and return
===================================
```

Depois:

```text
========== TEST 01 END ==========
Result: SUCCESS
=================================
```

ou:

```text
========== TEST 02 END ==========
Result: FAILURE
=================================
```

O resultado pode inicialmente ser marcado manualmente pelo tester.

---

# 37. Testes recomendados

Executar cada cenário várias vezes.

## Teste A

```text
Abrir jogo
→ entrar no gameplay
→ Alt+Tab
→ esperar 2 segundos
→ voltar
```

## Teste B

```text
Alt+Tab
→ esperar 10 segundos
→ voltar
```

## Teste C

```text
Alt+Tab
→ clicar em outra janela
→ voltar
```

## Teste D

```text
Alt+Tab repetidamente
```

## Teste E

```text
Win key
→ voltar
```

## Teste F

```text
Ctrl+Alt+Del
→ cancelar
→ voltar
```

## Teste G

```text
Alt+Tab em menu
```

## Teste H

```text
Alt+Tab durante gameplay
```

Isso ajuda a descobrir se a falha depende do estado interno do jogo.

---

# 38. Comparação mais importante

O diagnóstico deve buscar:

```text
ALT+TAB QUE FUNCIONOU
```

contra:

```text
ALT+TAB QUE FALHOU
```

Exemplo:

## Funcionou

```text
WM_SETFOCUS
TestCooperativeLevel -> D3DERR_DEVICENOTRESET
Reset -> D3D_OK
Mouse Acquire -> DI_OK
ClipCursor -> OK
Present -> D3D_OK
```

## Falhou

```text
WM_SETFOCUS
TestCooperativeLevel -> D3DERR_DEVICENOTRESET
Reset -> D3D_OK
Mouse Acquire -> DIERR_OTHERAPPHASPRIO
ClipCursor -> not called
Present -> D3D_OK
```

A primeira divergência relevante seria:

```text
Mouse Acquire
```

Isso direcionaria a investigação.

---

# 39. Exemplos de diagnósticos possíveis

## Caso 1: problema de foco da janela

```text
WM_ACTIVATEAPP TRUE

GetForegroundWindow != GameHWND
```

Possível área:

```text
Win32 window activation
```

---

## Caso 2: device permanece perdido

```text
WM_SETFOCUS

TestCooperativeLevel -> D3DERR_DEVICELOST
TestCooperativeLevel -> D3DERR_DEVICELOST
...
```

Possível área:

```text
Direct3D lost-device recovery
```

---

## Caso 3: Reset falha

```text
TestCooperativeLevel -> D3DERR_DEVICENOTRESET
Reset -> ERROR
```

Possível área:

```text
Direct3D presentation parameters
fullscreen state
display mode
resource lifecycle
```

---

## Caso 4: renderização não volta

```text
Reset -> D3D_OK

nenhum Present posterior
```

Possível área:

```text
game loop
render loop
application active state
```

---

## Caso 5: input não volta

```text
Reset -> D3D_OK
Present -> D3D_OK
Mouse Acquire -> ERROR
```

Possível área:

```text
DirectInput recovery
```

---

## Caso 6: clipping incorreto

```text
Mouse Acquire -> DI_OK
ClipCursor -> wrong rect
```

Possível área:

```text
cursor clipping / viewport calculation
```

---

## Caso 7: SetCursorPos incorreto

```text
ClipCursor -> correct
SetCursorPos -> outside client area
```

Possível área:

```text
coordinate conversion
resolution assumptions
window/client coordinates
```

---

# 40. Segunda fase: overlay

Somente depois que o logger estiver estável, pode ser adicionado um overlay opcional.

Exemplo:

```text
FOCUS       OK
D3D DEVICE  OK
PRESENT     OK
MOUSE       LOST
KEYBOARD    OK
CLIP        OK
```

O overlay deve poder ser completamente desligado.

Configuração:

```ini
[Diagnostics]
Logging=1
Overlay=0
```

---

# 41. Overlay não deve ser requisito

O modo de log precisa funcionar independentemente do overlay.

Ideal:

```text
Logging = ON
Overlay = OFF
```

por padrão.

Se o overlay causar alguma alteração no comportamento do problema, ele pode simplesmente ser desativado.

---

# 42. Arquivo de configuração opcional

Exemplo:

```ini
[Diagnostics]

LogWindowMessages=1
LogD3D8=1
LogDirectInput=1
LogCursor=1

LogEveryPresent=0
LogGetDeviceState=0

ManualSnapshotKey=F11
MarkerKey=F10

Overlay=0
```

Isso permite aumentar ou reduzir a instrumentação sem recompilar.

---

# 43. Níveis de log

Pode ser útil utilizar:

```text
ERROR
WARN
INFO
TRACE
```

Exemplo:

```text
[INFO]  WM_SETFOCUS
[TRACE] SetCursorPos(960,540)
[WARN]  Mouse Acquire -> DIERR_OTHERAPPHASPRIO
[ERROR] Reset -> FAILED
```

---

# 44. Primeira versão mínima recomendada

A versão 0.1 não precisa implementar tudo deste documento.

Versão mínima:

### Inicialização

- carregar DLL;
- abrir arquivo de log;
- localizar janela principal.

### Window

- `WM_ACTIVATE`;
- `WM_ACTIVATEAPP`;
- `WM_SETFOCUS`;
- `WM_KILLFOCUS`;
- `WM_SIZE`.

### Direct3D 8

- `TestCooperativeLevel`;
- `Reset`;
- `Present`.

### DirectInput

- `Acquire`;
- `Unacquire`.

### Cursor

- `ClipCursor`;
- `SetCursorPos`.

### Diagnóstico

- timestamp;
- thread ID;
- contador de frames;
- snapshot manual.

Isso já deve ser suficiente para identificar grande parte dos problemas de recuperação de foco.

---

# 45. Ordem de implementação sugerida

## Etapa 1

Criar DLL que:

- carrega;
- inicia logger;
- grava um arquivo;
- fecha corretamente.

## Etapa 2

Adicionar monitoramento da janela.

## Etapa 3

Adicionar `ClipCursor` e `SetCursorPos`.

## Etapa 4

Capturar `IDirect3DDevice8`.

## Etapa 5

Hookar:

```text
TestCooperativeLevel
Reset
Present
```

## Etapa 6

Capturar dispositivos DirectInput.

## Etapa 7

Hookar:

```text
Acquire
Unacquire
```

## Etapa 8

Adicionar snapshot manual.

## Etapa 9

Realizar testes comparativos.

## Etapa 10

Somente depois disso começar a experimentar correções.

---

# 46. O que NÃO fazer na primeira versão

Não implementar automaticamente:

```text
SetForegroundWindow
SetFocus
BringWindowToTop
Reset extra
Acquire extra
ClipCursor forçado
SetCursorPos forçado
```

Não modificar estados do jogo sem necessidade.

Também evitar:

- alteração de FOV;
- alteração de resolução;
- patches de gameplay;
- melhorias de performance;
- correções não relacionadas;
- recursos de QoL.

A DLL de diagnóstico deve ter um objetivo muito específico.

---

# 47. Escopo

O escopo da ferramenta é:

> Investigar a sequência de perda e recuperação de foco do jogo e identificar se a falha acontece no gerenciamento da janela, Direct3D 8, DirectInput, renderização ou controle do cursor.

Não é:

> Criar uma DLL geral de modificações para o jogo.

Manter esse limite é importante para que os resultados do diagnóstico sejam confiáveis.

---

# 48. Resultado esperado

Ao final da investigação, devemos conseguir responder perguntas como:

```text
O Windows realmente devolveu foco para a janela?

O jogo recebeu WM_SETFOCUS?

WM_ACTIVATEAPP retornou para TRUE?

O device Direct3D ficou em D3DERR_DEVICELOST?

Ele chegou a D3DERR_DEVICENOTRESET?

Reset foi chamado?

Reset retornou D3D_OK?

Present voltou a ser executado?

O contador de frames voltou a avançar?

O mouse foi readquirido?

O teclado foi readquirido?

ClipCursor foi chamado novamente?

O retângulo de clipping estava correto?

SetCursorPos recebeu coordenadas corretas?

A janela do jogo era realmente a foreground window?
```

Se conseguirmos responder isso, o problema deixa de ser:

```text
"Alt+Tab às vezes não volta direito"
```

e passa a ser algo específico, por exemplo:

```text
"Após WM_SETFOCUS, o Direct3D é resetado corretamente e o jogo
volta a apresentar frames, mas o mouse retorna DIERR_OTHERAPPHASPRIO
e nunca é readquirido."
```

Nesse ponto já existe uma causa concreta para investigar.

---

# 49. Meta final

A DLL de diagnóstico deve transformar um problema subjetivo e difícil de reproduzir em uma sequência objetiva de eventos.

O objetivo não é tentar adivinhar qual componente está quebrando.

O objetivo é produzir evidência suficiente para descobrir:

```text
qual estado estava correto,
qual estado deveria mudar,
qual chamada deveria acontecer,
qual chamada realmente aconteceu,
qual foi o resultado,
e em qual ponto a recuperação deixou de seguir o caminho normal.
```

Depois dessa etapa, pode ser criada uma correção específica com risco muito menor de introduzir efeitos colaterais.

---

# 50. Resumo

Primeiro:

```text
OBSERVAR
```

Depois:

```text
COMPARAR
```

Depois:

```text
IDENTIFICAR A PRIMEIRA DIVERGÊNCIA
```

Somente então:

```text
CORRIGIR
```

Essa deve ser a filosofia da DLL.

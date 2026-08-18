# ClipCursor Recovery — proxy dinput8.dll

DLL proxy x86 dedicada exclusivamente a restaurar o confinamento de cursor de
jogos antigos depois de Alt-Tab e diagnosticar cada requisito dessa restauração.
Ela não intercepta DirectInput, Direct3D, `SetCursorPos`, `ShowCursor` ou
`SetCursor`, e não força foco ou ativação da janela.

## Compilar

1. No Visual Studio 2022, instale **Desenvolvimento para desktop com C++** e o
   Windows 10/11 SDK.
2. Abra `FocusDiagnostic.sln`.
3. Selecione **Release** e **x86**.
4. Use **Build > Build Solution**.

O resultado é `bin\Release\dinput8.dll`. A proxy encaminha as cinco exportações
de `dinput8.dll` para a biblioteca original do diretório de sistema; ela não
inspeciona nem modifica os objetos DirectInput retornados ao jogo.

## Instalar

Copie `bin\Release\dinput8.dll` e `bin\Release\FocusDiagnostic.ini` para a pasta
do executável do jogo. O jogo deve ser x86 e importar `dinput8.dll`. Não substitua
outra DLL com o mesmo nome sem antes identificar a qual mod ela pertence.

Quando carregada, a DLL cria `FocusDiagnostic.log` ao lado dela. Faça testes
somente em uma cópia local/offline do jogo; proxies podem ser sinalizadas por
sistemas anticheat.

## Configuração

```ini
[Recovery]
RestoreCursorClip=1
RestoreCursorClipDelayMs=250
WaitForDisplayChange=1
```

- `RestoreCursorClip=1` habilita a restauração. Use `0` para uma sessão passiva
  que registra o processo sem chamar `ClipCursor` para restaurar.
- `RestoreCursorClipDelayMs` define a estabilização após o retorno do foco ou a
  confirmação do modo de vídeo; o valor é limitado ao intervalo 1–10000 ms.
- `WaitForDisplayChange=1` espera o retorno das dimensões de tela anteriores ao
  Alt-Tab. Se `WM_DISPLAYCHANGE` não chegar, existe um fallback de dois segundos.

Não existem opções para DirectInput, Direct3D, visibilidade do cursor, captura ou
ativação forçada porque esses subsistemas não fazem parte desta DLL.

## Processo de restauração

1. O hook passivo de `ClipCursor` registra solicitações bem-sucedidas do jogo e
   consulta o retângulo realmente aplicado com `GetClipCursor`.
2. Na perda de foco, a DLL memoriza se havia clipping ativo, o último retângulo
   aplicado e as dimensões atuais da tela.
3. No retorno do foco, uma thread agendadora espera a confirmação do modo de
   vídeo ou o fallback sem depender de `WM_TIMER` nem da fila de mensagens do jogo.
4. Antes de agir, exige que a janela do jogo seja foreground, tenha foco, esteja
   visível e não esteja minimizada.
5. Ela compara o retângulo atual com o esperado. `ClipCursor` só é chamado se os
   retângulos forem diferentes.
6. Depois da chamada, `GetClipCursor` confirma se o retângulo esperado foi
   realmente aplicado.
7. Cada tentativa é identificada por uma geração e processada uma única vez.
   Rearmes, nova perda de foco e tentativas posteriores invalidam agendamentos
   anteriores. Em nova perda de foco, somente um clipping aplicado pela própria
   DLL é liberado.

Chamadas internas usam a função original diretamente e não são registradas como
novas solicitações do jogo.

## Diagnóstico

O log contém apenas informações necessárias para avaliar a restauração:

- chamadas de `ClipCursor`, resultado da chamada e retângulo real;
- `WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_SETFOCUS` e `WM_KILLFOCUS`;
- `WM_DISPLAYCHANGE` e comparação com as dimensões anteriores;
- instante do agendamento, prazo solicitado, atraso real e atraso excedente da
  thread agendadora;
- validação de foreground, foco, visibilidade e minimização;
- retângulos esperado, anterior e posterior;
- se a restauração era necessária, se a chamada funcionou e se foi confirmada;
- classificação de `ClipCursor(NULL)` como transição de foco ou liberação
  intencional, com foreground, foco, display e propriedade do clipping;
- snapshots manuais e automáticos de sucesso ou falha.

Durante o teste:

1. Entre no gameplay e pressione **F10** para inserir `USER MARKER`.
2. Faça Alt-Tab e retorne ao jogo.
3. Pressione **F11** para gravar um snapshot manual.
4. Feche normalmente o jogo e preserve `FocusDiagnostic.log`.

Uma tentativa bem-sucedida termina com `CLIP RESTORATION SUCCESS` e
`restored=YES`. Se nenhum clipping ativo foi observado antes da perda de foco, o
log informa `NO ACTIVE CLIP CAPTURED BEFORE FOCUS LOSS`; nesse caso não existe um
retângulo válido para restaurar.

Quando o jogo chama `ClipCursor(NULL)` antes das mensagens formais de perda de
foco, a DLL mantém o último retângulo ativo como candidato. A chamada é
classificada como `FOCUS_TRANSITION` somente quando foreground, foco ou mudança
de display comprovam a transição em até cinco segundos e não houve uma chamada
não nula posterior. Sem essa evidência, ela é classificada como
`INTENTIONAL_RELEASE` e não é restaurada.

## Limitações

- Apenas chamadas de `ClipCursor` importadas diretamente pelo executável
  principal são interceptadas. Chamadas feitas por módulos auxiliares ou obtidas
  por `GetProcAddress` não são observadas.
- A localização da janela considera a primeira janela superior, visível e sem
  proprietário pertencente ao processo.
- A DLL restaura confinamento, não visibilidade nem imagem do cursor.

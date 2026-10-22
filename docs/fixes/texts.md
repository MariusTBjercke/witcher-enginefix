# The mod's texts in the game's languages

The mod shows texts of its own: the pause menu's "Exit game" button (QoL 2), "Borderless window" and its tooltip in Options > Graphics (QoL 3), the Ctrl+F12 message (graphics detail) and, since 0.7.0, the main menu's "Continue" (QoL 7). Since 0.6.0 they follow the game's text language. Reported by Illuzio on Nexus (2026-10-20): in French both pause menu buttons read "QUITTER".

## Languages

`Data\languages.2da` lists the text languages; the Steam build ships a talk table (`Data\dialog_<id>.tlk`) for each of these ids: 3 English, 5 Polish, 10 German, 11 French, 12 Spanish, 13 Italian, 14 Russian, 15 Czech, 16 Hungarian, 21 Traditional Chinese. Any other id gets English.

- Lua: `g_pClientExoApp:GetClientTextLanguage()` returns the id (`startup.luc` prints it as "Current language:"; the journal stores it in `lm_nClientLanguage`).
- DLL: the registry value `TextLanguage` (REG_SZ, e.g. "3") under `HKCU\Software\CD Projekt RED\Witcher\Settings`, read when the message is shown.

## Why the game's own "Exit game" text was not enough

QoL 2 used StrRef 81 (`GuiLMMExit`, the main menu's Exit). In three languages it reads the same as the pause menu's "Exit to main menu" (2392), and in two more the main-menu button sounds like quitting:

| id | 81 (main menu Exit) | 2392 (pause menu "Exit to main menu") | the new button |
|---|---|---|---|
| 3 | EXIT GAME | EXIT TO MAIN MENU | EXIT GAME |
| 5 | OPUŚĆ GRĘ | OPUŚĆ GRĘ | WYJDŹ DO PULPITU |
| 10 | SPIEL VERLASSEN | SPIEL VERLASSEN | ZUM DESKTOP |
| 11 | QUITTER | QUITTER → RETOUR AU MENU PRINCIPAL | QUITTER LE JEU (Illuzio) |
| 12 | SALIR | SALIR DEL JUEGO | SALIR AL ESCRITORIO |
| 13 | ESCI DAL GIOCO | ABBANDONA PARTITA | ESCI DAL GIOCO |
| 14 | ВЫХОД | ВЫХОД В МЕНЮ | ВЫХОД ИЗ ИГРЫ |
| 15 | UKONČIT HRU | KONEC | UKONČIT HRU |
| 16 | KILÉPÉS | KILÉPÉS A JÁTÉKBÓL | KILÉPÉS A WINDOWSBA |
| 21 | 離開遊戲 | 離開回到主選單 | 離開遊戲 |

French is the one language where the mod also relabels the game's own "Exit to main menu" (`ExitButton`): next to "QUITTER LE JEU", a plain "QUITTER" did not say which button goes where. Illuzio's follow-up on Nexus (2026-10-20) suggested "RETOUR AU MENU PRINCIPAL" (the game's own phrase elsewhere is "Revenir au menu principal"). It is the longest pause-menu label (24 characters, the others are at most 19), so check in game that it fits the button.

"Borderless window" uses each language's word for the game's own "Full screen" setting (StrRef 1045: Plein écran, Vollbild, Pełny ekran, Во весь экран, 全螢幕, ...). The French texts ("Fenêtre sans bordure" and the tooltip "Lorsque le mode plein écran est désactivé, ...") are Illuzio's.

## How the texts reach the screen

- Strings in the `.tlk` files are UTF-8. `g_TalkTable:GetSimpleString` (binding at `0x00644a60` → `FUN_0081c960` → `FUN_0081bd80`) copies the raw bytes into the returned string, without conversion. The options panel passes that string to `CTextControl:InitTextLayer(nil, text, font)` (`gui_new_system_settingspanel.luc`), and that is how Polish, Russian and Chinese setting names display. So a UTF-8 Lua string displays the same way.
- Button labels: `defineGUIPanel` (`cdefineguipanel.luc`) takes `String.StrRef` (a `g_tsGuiLabels` key, a StrRef number turned into text by `processItem`) or, when that is nil, `String.String`, a literal passed to `InitTextLayer` as is. The "Exit game" definition clears `StrRef` and sets `String`.
- Tooltip: `g_tSettingsDescriptions[key]` goes through `processItem` to `ShowTooltipText`, which already took the English text.
- The languages use their own fonts (`languages.2da`: `fonts_rus` for Russian, `fonts_zh` for Chinese), which may only hold the characters the game uses. `tools\check_texts.py <game folder>` checks every character of `TEXTS` against that language's `.tlk`. The Chinese talk table has 1729 distinct characters and lacks 框 and 填, so "borderless" is 無邊界視窗, not 無邊框視窗.
- The Ctrl+F12 message is drawn by the DLL with `D3DXCreateFontW` + `DrawTextW` (vtable 15) and a Windows font: Georgia (Latin, Cyrillic), Microsoft JhengHei for Chinese. A test render of all ten messages showed no missing glyphs.

## Adding or fixing a translation

Edit `TEXTS` in `mod\System\Scripts\enginefix_hook.lua` (UTF-8, no BOM) and `kDetailTexts` in `src\enginefix.c` (written as `\uXXXX` escapes, because MSVC reads a source file without BOM in the ANSI code page), then run `python -I tools\check_texts.py <game folder>`.

## Verification

Lua 5.1 stub test: each id picks its texts, unknown ids, 0 and a failing `GetClientTextLanguage` get English, and the original "Exit to main menu" definition is left alone. `check_texts.py`: OK for all ten. In game: pending (switch the text language in Options, restart, look at the pause menu, Options > Graphics and Ctrl+F12).

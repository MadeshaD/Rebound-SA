# Rebound

Rebound 0.59 — GTA San Andreas classic PC 1.0 US

Установка. Поместите папку Rebound в modloader. Оставьте ASI, INI и phonon.dll вместе. Геометрию плагин получает из загруженных игровых коллизий.

Управление. F8 перечитывает INI. После смены ReloadKey первый раз нажмите прежнюю клавишу. General / Enabled применяется при запуске игры. Effects / Echo управляет ранними отражениями, Effects / Reverb — поздним хвостом вместе с гулом. Оба переключателя действуют независимо.

Прямой звук. Поддерживаемые позиционные буферы используют Steam Audio. Короткие буферы и каналы, для которых DirectSound не смог подключить эффект, сохраняют совместимую обработку. Плагин учитывает материалы, частичное перекрытие, пути вокруг препятствий, поглощение звука воздухом и направленность распознанных источников. Доплер применяется к поддерживаемым вызовам игры.

Сборка. Запустите source/build.sh с 32-битным MinGW-w64. Сведения о сторонних компонентах приведены в THIRDPARTY.md.

0.59. Исправлено распознавание стадионов при Interiors=0: проверяются ID моделей stadint и соответствующий код интерьера вместо индекса IPL. Проверочные лучи игнорируют физические объекты, машины и пешеходов. В журнал добавлен stadiumModel — ID последней распознанной модели стадиона. Игровые файлы IDE/IPL копировать в папку плагина не нужно. General / Logging=1 включает журнал состояния основных функций, обновляемый раз в пять секунд. F8 применяет изменение. Logging=0 выключает запись. По умолчанию запись отключена. Манифест встроен в ASI, отдельный файл манифеста для установки не требуется.

## Complete package

This package contains the compiled `Rebound.SA.asi`, default `Rebound.SA.ini`, Windows x86 `phonon.dll`, source code and dependency licenses for version 0.59. The rename does not change audio processing.

Build on Linux or in an MSYS2 environment with the 32-bit MinGW-w64 C++ compiler and windres:

```sh
bash source/build.sh
```

The output is `Rebound.SA.asi` in the repository root. Steam Audio headers are included; no SDK import library is needed because the plugin loads the API dynamically. At runtime, place the Windows x86 `phonon.dll` from Steam Audio SDK 4.8.1 beside the ASI and INI. The required SDK runtime binary is included in this archive. Preserve `SteamAudio-License.txt` and `THIRDPARTY.md` when distributing the runtime library.

The DirectSound effect manifest is embedded by the build script. Keep its COM class identifier unchanged: the effect implementation uses the same identifier.

When upgrading, remove the old plugin ASI to avoid loading both copies, and rename your existing settings file to `Rebound.SA.ini` if you want to retain your settings. Logging uses `Rebound.SA.log`; F8 reloads the INI by default.

Native stadium classification check:

```sh
c++ -std=c++17 source/tests/stadium_test.cpp -o /tmp/rebound-stadium-test
/tmp/rebound-stadium-test
```

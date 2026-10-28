# Подготовка среды — 2026-10-27

## Исходное окружение

- CLion 2025.2.2: встроенный GCC 13.1.0, CMake 4.0.2, Ninja 1.12.1.
- Python 3.12.10; Git 2.53.0.windows.2.
- Компиляторы, CMake и Ninja отсутствовали в PATH.
- MSYS2 отсутствовал в C:\msys64; Goose отсутствовал в PATH.

## Установлено

- MSYS2 Installer 20260927 через winget, C:\msys64.
  winget подтвердил контрольную сумму установщика.
- Goose v2026.3, официальный архив goose-v2026.3-windows-x64.zip.
  Каталог C:\tools\goose\goose-v2026.3-windows-x64.
  Локально вычисленный SHA-256 архива:
  E30CFA398C0362CEF4B8F311CF5496743920B6D3A6C4A6D818FC47EE1E56BE7D.
  Это фиксация скачанного файла, не независимая проверка издателя.
- `goose version`: goose runtime 4d3da949e563.
- `goose goose/hello.goose`: успешно, Hello from FeedGuard Goose.

## Установленные версии

- GCC 16.2.0; Clang 23.1.4.
- CMake 4.4.4; Ninja 1.13.2.
- Boost 1.92.0; OpenSSL 3.6.5; simdjson 5.0.5; Catch2 3.16.1.
- Библиотеки установлены отдельно в mingw64 и clang64.
- repo.msys2.org установлен первым зеркалом с отдельного согласия пользователя;
  исходные mirrorlist.msys и mirrorlist.mingw сохранены с `.feedguard-original`.

## Результаты проверки этапа 1

| Профиль | Сборка | CTest | Запуск трёх приложений |
|---|---|---|---|
| gcc-release | успешно | 3/3 | успешно |
| clang-release | успешно | 3/3 | успешно |
| gcc-debug | успешно | 3/3 | успешно |
| clang-debug | успешно | 3/3 | успешно |

Проверки: параметры рынка, разбор JSON через simdjson,
создание клиентского TLS-контекста OpenSSL.
Это проверки каркаса и зависимостей, а не проверки соединения с Bybit.

Goose успешно собрал `goose/hello.exe` через Clang 23.1.4 с `-O2`;
exe вывел `Hello from FeedGuard Goose` и завершился с кодом 0.
Для проверки кэш Goose находился в `build/goose-home` через GOOSE_HOME.
В verbose-выводе подтверждён вызов `C:/msys64/clang64/bin/clang.exe -O2`
и для сгенерированного C, и для runtime.

В интерфейсе CLion профили пока не открывались. Проверена сборка через
те же CMake presets из PowerShell.

Постоянный системный PATH не изменён. README задаёт PATH в текущем PowerShell;
CMake presets задают окружение отдельно для каждого toolchain.

@echo off
echo Building mlbrowser for Windows...

:: Проверка наличия gcc (MinGW)
where gcc >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: gcc not found. Please install MinGW-w64 and add it to PATH.
    pause
    exit /b
)

:: Собираем через make, если он есть, иначе напрямую через gcc
where make >nul 2>nul
if %errorlevel% == 0 (
    make clean
    make
) else (
    echo 'make' not found, trying manual compilation...

    set CFLAGS=-Wall -Wextra -O2 -DHAVE_RENDER -DHAVE_UI -DHAVE_LOGGER -DHAVE_NET
    set LIBS=-lgdi32 -luser32 -lws2_32 -lz -lsqlite3 -lm

    :: Собираем список всех C файлов
    set SRCS=main.c gal_common.c gal_win32.c

    :: Добавляем файлы из модов (ручной поиск для cmd)
    for /r mods %%f in (*.c) do (
        call set SRCS=%%SRCS%% %%f
    )

    gcc %CFLAGS% %SRCS% -o my_browser.exe %LIBS%
)

if %errorlevel% == 0 (
    echo Build successful! Starting my_browser.exe...
    my_browser.exe
) else (
    echo Build failed.
)

pause

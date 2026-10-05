#!/bin/bash

echo "Автоматическая настройка среды для кросс-компиляции Win32..."

# Функция для проверки наличия пакетного менеджера
has_cmd() {
    command -v "$1" >/dev/null 2>&1
}

if has_cmd pacman; then
    echo "Обнаружен Arch Linux (pacman)"
    sudo pacman -S --needed --noconfirm mingw-w64-gcc wine make pkgconf base-devel
elif has_cmd dnf; then
    echo "Обнаружена Fedora/RHEL (dnf)"
    sudo dnf install -y mingw64-gcc wine make pkgconf-pkg-config
elif has_cmd apt-get; then
    echo "Обнаружен Debian/Ubuntu (apt-get)"
    sudo apt-get update
    sudo apt-get install -y mingw-w64 wine build-essential pkg-config
else
    echo "Ошибка: Не удалось определить пакетный менеджер (поддерживаются pacman, dnf, apt-get)."
    echo "Пожалуйста, установите mingw-w64, wine, make и pkg-config вручную."
    exit 1
fi

echo "-------------------------------------------------------"
echo "Установка завершена!"
echo "-------------------------------------------------------"
echo "Сборка под Windows (из Linux):  make CROSS=win32"
echo "Запуск в Wine:                 make CROSS=win32 test_wine"
echo "-------------------------------------------------------"

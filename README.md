# ECHO Native (Linux)

Нативный лаунчер на **Qt 6** и **SDL3** для установки текстового перевода Wuthering Waves через Steam/Proton на CachyOS и Arch Linux.

## Установка

Скачайте `release/echo-native-0.2.1-1-x86_64.pkg.tar.zst` из приватного репозитория:

```bash
sudo pacman -U ./echo-native-0.2.1-1-x86_64.pkg.tar.zst
```

Также рядом с пакетом доступен `release/install-echo-native.sh` (polkit при наличии).
Приложение появляется в меню: **ECHO Native**.
Зависимости: `qt6-base`, `sdl3`, `xxhash`.

## Управление геймпадом

- D-pad / левый стик — навигация.
- A — выбрать или подтвердить.
- B — назад.
- LB/RB — вкладки «Перевод» / «Настройки».
- Правый стик — прокрутка.

SDL3 отслеживает подключение/отключение контроллера; когда окно неактивно, ввод игнорируется.

## Сборка

Требуются CMake, C++17, Qt6 Widgets+Network, SDL3, xxhash и pkgconf.

```bash
cmake -S native -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j3
QT_QPA_PLATFORM=offscreen build/native/echo-native --ui-test
QT_QPA_PLATFORM=offscreen build/native/echo-native --controller-test
bash scripts/build-arch-package.sh
```

Готовый пакет появляется в `release/`. Сборка с помощью `makepkg` не включает игровые файлы.

## Ограничения

Лаунчер не содержит ECHO.exe, игровых файлов, чужих PAK/SIG или механизмов обхода античита. Не гарантирует отсутствие вылетов, обнаружения модификаций или блокировок аккаунта. При изменении Mount создаёт резервные копии.

Физический геймпад следует отдельно проверить на устройстве: на Zenith автоматические тесты навигации проходят, но контроллер не подключён.

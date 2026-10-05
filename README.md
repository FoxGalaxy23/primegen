# Meetlookers Browser Engine

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-1.0.0-green.svg)

**Meetlookers** is a lightweight, modular browser engine written in C, designed with a microservices-like architecture. It features its own rendering pipeline, JavaScript execution environment, and platform abstraction layers.

## 🚀 Key Features

- **Microservices-inspired Architecture**: Every component is a self-contained module with its own manifest and build logic.
- **Universal Build System**: Supports native Linux (X11/Wayland) and cross-compilation for Windows.
- **Modular Abstraction Layers**:
  - **GAL (Graphic Abstraction Layer)**: Pluggable backends for X11, Wayland, and Win32.
  - **NAL (Network Abstraction Layer)**: Abstracted HTTP/HTTPS handling.
- **Custom Rendering Pipeline**: Integrated HTML parser, CSS engine, and TTF font rendering.
- **Embedded JS Engine**: Support for scripting and DOM manipulation.

---

## 🏗 Architecture

The project is structured into functional categories under the `mods/` directory:

```text
mods/
├── frame/          # Core browser logic (UI, JS, DB, Network)
├── render/         # Visual pipeline (Parser, Layout, CSS, Fonts)
├── gal/            # Graphics Abstraction Layer
└── nal/            # Network Abstraction Layer
```

Each module contains:
- `start.c`: Main implementation.
- `module.mk`: Module build configuration (Make-compatible).
- `manifest`: Module metadata.

---

## 🛠 Building and Packaging

### Requirements
- **Linux**: `gcc`, `make`, `libx11-dev`, `libfreetype6-dev`, `libpng-dev`, `sqlite3`.
- **Windows (Cross-compile)**: `mingw-w64`.

### Compilation

Build the default version (Linux X11):
```bash
make
```

Build for Wayland:
```bash
make PLATFORM=wayland
```

Cross-compile for Windows:
```bash
make CROSS=win32
```

### Packaging

Generate a distribution-ready archive in the `compile/` directory:
```bash
make package
```
*Note: This will strip symbols and include all necessary assets and cache structures.*

---

## ⚙️ Module Management

### Disabling Modules
You can exclude modules at build-time using the `DISABLED_MODS` variable:
```bash
make DISABLED_MODS="logger db"
```

### Adding a New Module
1. Create a directory in `mods/frame/` or `mods/render/`.
2. Add your source files (e.g., `my_module.c`).
3. Create a `module.mk`:
   ```makefile
   MOD_NAME := my_module
   MOD_SRCS += \$(MOD_PATH)/my_module.c
   ```
4. The build system will automatically discover and include it.

---

## 📄 License
This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

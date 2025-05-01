b development (with # Ceditr

Ceditr is a lightweight, terminal-based text editor written in C, inspired by the Kilo editor. It features syntax highlighting for C/C++ files, efficient file editing, and a simple, intuitive interface. Designed for Unix-like systems, Ceditr is perfect for developers who want a minimal yet powerful editing experience.

## Features

- **Syntax Highlighting**: Supports C/C++ with highlighting for keywords, strings, numbers, and comments.
- **Efficient Editing**: Handles basic operations like insert, delete, and navigation with minimal resource usage.
- **Search Functionality**: Incremental search with navigation using arrow keys.
- **File I/O**: Open, edit, and save files with dirty state tracking to prevent accidental data loss.
- **Terminal Integration**: Uses raw terminal mode for responsive input and ANSI escape codes for rendering.
- **Customizable**: Easily extensible syntax highlighting framework for adding more languages.
b development (with 
## Installation

### Prerequisites
- A Unix-like system (Linux, macOS, or BSD).
- A C compiler (e.g., `gcc` or `clang`).
- Standard C library (available on most systems).

### Build Instructions
1. Clone the repository:
   ```bash
   git clone https://github.com/CFrancis03/ceditr.git
   cd ceditr
   ```
2. Compile the source code:
   ```bash
   gcc -o ceditr ceditr.c -Wall
   ```
3. (Optional) Install the binary to `/usr/local/bin` for system-wide access:
   ```bash
   sudo cp ceditr /usr/local/bin/
   ```

## Usage

Run Ceditr from the terminal, optionally specifying a file to open:

```bash
./ceditr [filename]
```

### Keybindings
- **Ctrl-S**: Save the current file.
- **Ctrl-Q**: Quit (press multiple times if there are unsaved changes).
- **Ctrl-F**: Search for text (use arrows to navigate matches, ESC to cancel).
- **Arrow Keys**: Move the cursor.
- **Home/End**: Jump to start/end of line.
- **Page Up/Down**: Scroll one screen at a time.
- **Backspace/Delete**: Delete characters.
- **Enter**: Insert a new line.

### Example
To edit a C file:
```bash
./ceditr example.c
```
The editor will open with syntax highlighting, and you can start editing immediately.


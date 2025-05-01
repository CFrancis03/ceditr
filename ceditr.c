// Ceditr: A lightweight terminal-based text editor with syntax highlighting
// Inspired by the Kilo editor, with added features like C syntax highlighting
// and search functionality. Supports basic editing, file I/O, and navigation.

// Standard library includes for string manipulation, I/O, and terminal control
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <termios.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

// Macro definitions for editor configuration and key handling
#define CTRL_KEY(k) ((k) & 0x1f) // Convert key to Ctrl-key equivalent
#define CEDITR_TAB_STOP 8        // Number of spaces per tab
#define CEDITR_QUIT_TIMES 3      // Times Ctrl-Q must be pressed to quit with unsaved changes
#define CEDITR_VERSION "0.0.1"   // Editor version

// Enum for special keys not represented by single characters
enum editorKey {
    BACKSPACE = 127, // ASCII code for backspace
    ARROW_LEFT = 1000, // Custom codes for arrow keys and other special keys
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

// Enum for syntax highlighting types
enum editorHighlight {
    HL_NORMAL = 0, // Default text
    HL_COMMENT,    // Single-line comments
    HL_MLCOMMENT,  // Multi-line comments
    HL_KEYWORD1,   // Primary keywords (e.g., if, while)
    HL_KEYWORD2,   // Secondary keywords (e.g., int, char)
    HL_STRING,     // String literals
    HL_NUMBER,     // Numeric literals
    HL_MATCH       // Search matches
};

// Flags for syntax highlighting options
#define HL_HIGHLIGHT_NUMBERS (1<<0) // Enable number highlighting
#define HL_HIGHLIGHT_STRINGS (1<<1) // Enable string highlighting

// Data structures

// Syntax definition for a file type
struct editorSyntax {
    char *filetype;                // Name of the file type (e.g., "c")
    char **filematch;             // Array of file extensions or patterns
    char **keywords;              // Array of keywords to highlight
    char *singleline_comment_start;// Start of single-line comments (e.g., "//")
    char *multiline_comment_start; // Start of multi-line comments (e.g., "/*")
    char *multiline_comment_end;   // End of multi-line comments (e.g., "*/")
    int flags;                    // Highlighting options (e.g., numbers, strings)
};

// Editor row, representing a line of text
typedef struct erow {
    int idx;           // Row index in the file
    int size;          // Length of chars
    int rsize;         // Length of rendered chars (after tab expansion)
    char *chars;       // Raw characters
    char *render;      // Rendered characters (tabs expanded to spaces)
    unsigned char *hl; // Highlighting array (one byte per render char)
    int hl_open_comment;// Flag for open multi-line comment
} erow;

// Global editor configuration
struct editorConfig {
    int cx, cy;           // Cursor position (x, y)
    int rx;               // Rendered cursor x position (accounts for tabs)
    int rowoff;           // Row offset for scrolling
    int coloff;           // Column offset for scrolling
    int screenrows;       // Number of rows in the terminal
    int screencols;       // Number of columns in the terminal
    int numrows;          // Number of rows in the file
    erow *row;            // Array of rows
    int dirty;            // Flag for unsaved changes
    char *filename;       // Current file name
    char statusmsg[80];   // Status message text
    time_t statusmsg_time;// Timestamp for status message
    struct editorSyntax *syntax; // Current syntax highlighting rules
    struct termios orig_termios; // Original terminal settings
};

// Global editor state
struct editorConfig E;

// Filetype definitions for C/C++ syntax highlighting
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL // Keywords ending in '|' are HL_KEYWORD2 (types)
};

// Syntax highlighting database
struct editorSyntax HLDB[] = {
    {
        "c",                     // Filetype
        C_HL_extensions,         // Extensions
        C_HL_keywords,           // Keywords
        "//", "/*", "*/",        // Comment markers
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS // Flags
    }
};

#define HLDB_ENTRIES (sizeof(HLDB)/sizeof(HLDB[0])) // Number of syntax entries

// Function prototypes
void editorSetStatusMessage(const char *fmt, ...); // Set status bar message
void editorDelRow(int at);                        // Delete a row
void editorRowAppendString(erow *row, char *s, size_t len); // Append string to row
void editorRefreshScreen();                       // Redraw the screen
char *editorPrompt(char *prompt, void (*callback)(char *, int)); // Prompt user input

// Terminal handling

// Exit with an error message and print errno description
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
    write(STDOUT_FILENO, "\x1b[H", 3);  // Move cursor to top-left
    perror(s);
    exit(1);
}

// Restore original terminal settings
void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

// Enable raw terminal mode for character-by-character input
void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode); // Ensure raw mode is disabled on exit

    struct termios raw = E.orig_termios;
    // Disable input flags: break, CR-to-NL, parity, 8th-bit strip, flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST); // Disable output processing
    raw.c_cflag |= (CS8);    // Set 8 bits per byte
    // Disable local flags: echo, canonical mode, extended input, signals
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;  // Minimum bytes to read
    raw.c_cc[VTIME] = 1; // Timeout in deciseconds
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// Read a single keypress, handling escape sequences for special keys
// Returns: The key code (ASCII or enum value for special keys)
int editorReadKey() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (c == '\x1b') { // Escape sequence
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') { // CSI sequence
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') { // VT100-style keys
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else { // Arrow and other keys
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') { // SS3 sequence
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

// Get current cursor position using ANSI escape codes
// Parameters: rows, cols - pointers to store row and column
// Returns: 0 on success, -1 on failure
int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1; // Request position
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

// Get terminal window size
// Parameters: rows, cols - pointers to store rows and columns
// Returns: 0 on success, -1 on failure
int getWindowSize(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Fallback: move cursor to bottom-right and query position
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

// Syntax highlighting

// Check if a character is a separator (for keyword detection)
// Parameters: c - character to check
// Returns: 1 if separator, 0 otherwise
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

// Update syntax highlighting for a row
// Parameters: row - pointer to the row to update
void editorUpdateSyntax(erow *row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize); // Default to normal highlighting
    if (E.syntax == NULL) return; // No syntax rules

    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->singleline_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1; // Track if previous char was a separator
    int in_string = 0; // Track if inside a string literal
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment); // Check for open comment

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i-1] : HL_NORMAL;

        // Handle single-line comments
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i); // Highlight rest of line
                break;
            }
        }

        // Handle multi-line comments
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len); // Highlight end marker
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                }
                i++;
                continue;
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len); // Highlight start marker
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // Handle strings
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i+1] = HL_STRING; // Highlight escaped char
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0; // End of string
                i++;
                prev_sep = 1;
                continue;
            } else if (c == '"' || c == '\'') {
                in_string = c; // Start of string
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        // Handle numbers
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // Handle keywords
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|'; // Type keywords end with '|'
                if (kw2) klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }
        prev_sep = is_separator(c);
        i++;
    }
    // Update subsequent rows if comment state changed
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows)
        editorUpdateSyntax(&E.row[row->idx + 1]);
}

// Map highlighting type to ANSI color code
// Parameters: hl - highlight type
// Returns: ANSI color code
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36; // Cyan
        case HL_KEYWORD1:  return 33; // Yellow
        case HL_KEYWORD2:  return 32; // Green
        case HL_STRING:    return 35; // Magenta
        case HL_NUMBER:    return 31; // Red
        case HL_MATCH:     return 34; // Blue
        default:           return 37; // White
    }
}

// Select syntax highlighting based on file extension
void editorSelectSyntaxHighlight() {
    E.syntax = NULL;
    if (E.filename == NULL) return;
    char *ext = strrchr(E.filename, '.');
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;
                // Apply syntax to all rows
                for (int filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

// Row operations

// Convert cursor x position to rendered x position (accounts for tabs)
// Parameters: row - pointer to row, cx - cursor x position
// Returns: Rendered x position
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (CEDITR_TAB_STOP - 1) - (rx % CEDITR_TAB_STOP);
        rx++;
    }
    return rx;
}

// Convert rendered x position to cursor x position
// Parameters: row - pointer to row, rx - rendered x position
// Returns: Cursor x position
int editorRowRxToCx(erow *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (CEDITR_TAB_STOP - 1) - (cur_rx % CEDITR_TAB_STOP);
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

// Update row's rendered content and syntax highlighting
// Parameters: row - pointer to row to update
void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;
    free(row->render);
    // Allocate space for rendered chars (tabs expanded)
    row->render = malloc(row->size + tabs * (CEDITR_TAB_STOP - 1) + 1);
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % CEDITR_TAB_STOP) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
    editorUpdateSyntax(row);
}

// Insert a new row at the specified position
// Parameters: at - index to insert at, s - string content, len - length of string
void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;
    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

// Insert a character into a row
// Parameters: row - pointer to row, at - position to insert, c - character
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

// Delete a character from a row
// Parameters: row - pointer to row, at - position to delete
void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

// Free memory for a row
// Parameters: row - pointer to row
void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

// Delete a row
// Parameters: at - index of row to delete
void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

// Append a string to a row
// Parameters: row - pointer to row, s - string to append, len - string length
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// Editor operations

// Insert a character at the cursor position
// Parameters: c - character to insert
void editorInsertChar(int c) {
    if (E.cy == E.numrows)
        editorInsertRow(E.numrows, "", 0); // Add new row if at end
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// Insert a new line at the cursor position
void editorInsertNewLine() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0); // Insert empty row
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy]; // Re-fetch row (may have been reallocated)
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

// Delete character at or before cursor
void editorDelChar() {
    if (E.cy == E.numrows) return; // No rows to delete from
    if (E.cx == 0 && E.cy == 0) return; // Nothing to delete
    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

// File I/O

// Convert all rows to a single string for saving
// Parameters: buflen - pointer to store buffer length
// Returns: Allocated buffer containing file contents
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1; // Add 1 for newline
    *buflen = totlen;
    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

// Open and load a file into the editor
// Parameters: filename - name of file to open
void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    editorSelectSyntaxHighlight();
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        if (errno == ENOENT) {
            editorSetStatusMessage("New file: %s", filename);
            return;
        }
        die("fopen");
    }
    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

// Save the current file
void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }
    int len;
    char *buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

// Find functionality

// Callback for search prompt, handles search navigation
// Parameters: query - search string, key - keypress
void editorFindCallback(char *query, int key) {
    static int last_match = -1; // Last matched row
    static int direction = 1;   // Search direction (1 = forward, -1 = backward)
    static int saved_hl_line;   // Row with highlighted match
    static char *saved_hl = NULL; // Saved highlight state

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }
    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }
    if (last_match == -1) direction = 1;
    int current = last_match;
    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;
        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

// Prompt user for search query and handle search
void editorFind() {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

// Input handling

// Prompt user for input, with optional callback for each keypress
// Parameters: prompt - prompt string, callback - function to call per keypress
// Returns: Input string or NULL if cancelled
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();
        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

// Move cursor based on keypress
// Parameters: key - key code (e.g., ARROW_LEFT)
void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
    }
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

// Process a single keypress
void editorProcessKeypress() {
    static int quit_times = CEDITR_QUIT_TIMES; // Track quit attempts
    int c = editorReadKey();
    switch (c) {
        case '\r': // Enter key
            editorInsertNewLine();
            break;
        case CTRL_KEY('q'): // Quit
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4); // Clear screen
            write(STDOUT_FILENO, "\x1b[H", 3);  // Move cursor to top
            exit(0);
            break;
        case CTRL_KEY('s'): // Save
            editorSave();
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;
        case CTRL_KEY('f'): // Search
            editorFind();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) E.cy = E.numrows;
            }
            for (int times = E.screenrows; times--;)
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'): // Refresh (ignored)
        case '\x1b':        // Escape (ignored)
            break;
        default:
            editorInsertChar(c);
            break;
    }
    quit_times = CEDITR_QUIT_TIMES; // Reset quit counter
}

// Append buffer for efficient output

struct abuf {
    char *b; // Buffer data
    int len; // Buffer length
};

#define ABUF_INIT {NULL, 0} // Initial empty buffer

// Append string to append buffer
// Parameters: ab - append buffer, s - string, len - string length
void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// Free append buffer
// Parameters: ab - append buffer
void abFree(struct abuf *ab) {
    free(ab->b);
}

// Output handling

// Draw the message bar at the bottom of the screen
// Parameters: ab - append buffer
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3); // Clear line
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen); // Show message for 5 seconds
}

// Set status bar message with format string
// Parameters: fmt - format string, ... - arguments
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

// Draw the status bar
// Parameters: ab - append buffer
void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); // Invert colors
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen); // Right-aligned status
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3); // Reset colors
    abAppend(ab, "\r\n", 2);
}

// Update scroll position based on cursor
void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy; // Scroll up
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1; // Scroll down
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx; // Scroll left
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1; // Scroll right
    }
}

// Draw all rows to the screen
// Parameters: ab - append buffer
void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                // Show welcome message for empty file
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Ceditr -- version %s", CEDITR_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1); // Empty row marker
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            char *c = &E.row[filerow].render[E.coloff];
            unsigned char *hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    // Display control characters
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5); // Reset color
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5); // Reset color
        }
        abAppend(ab, "\x1b[K", 3); // Clear rest of line
        abAppend(ab, "\r\n", 2);
    }
}

// Refresh the entire screen
void editorRefreshScreen() {
    editorScroll();
    struct abuf ab = ABUF_INIT;
    abAppend(&ab, "\x1b[?25l", 6); // Hide cursor
    abAppend(&ab, "\x1b[H", 3);    // Move cursor to top-left
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);
    // Move cursor to correct position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6); // Show cursor
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// Initialization

// Initialize editor state
void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.syntax = NULL;
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; // Reserve space for status and message bars
}

// Main entry point
int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]); // Open file if provided
    }
    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = search");
    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}

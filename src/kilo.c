/*** includes ***/

// Feature test macro
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

// bitwise AND Ctrl-key with a given character
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
    char* filetype;
    char** filematch;
    char** keywords;
    char* singleline_comment_start;
    char* multiline_comment_start;
    char* multiline_comment_end;
    int flags;
};

// Data type for storing a row of text
typedef struct erow {
    int idx;
    int size;
    int rsize;
    char* chars;
    char* render;
    unsigned char* hl;
    int hl_open_comment;
} erow;

struct editorConfig {
    int cx, cy;             // Absolute cursor x and y position
    int rx;                 // Rendered cursor x position, to account for tabs
    int rowoff;             // Row offset into file
    int coloff;             // Column offset
    int screenrows;         // Number of rows on screen
    int screencols;         // Number of columns on screen

    int numrows;            // Number of rows in the file
    erow* row;              // Array of rows of text

    char* filename;         // Name of open file
    int dirty;              // Dirty bit: has file been edited?

    char statusmsg[80];     // Status bar message string
    time_t statusmsg_time;  // Current time

    struct editorSyntax* syntax;    // Syntax highlighting rules

    struct termios orig_termios;    // Settings to be restored after exiting raw mode
};

// Global container for editor state
struct editorConfig E;

/*** filetypes ***/

char* C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

char* C_HL_keywords[] = {
  "switch", "if", "while", "for", "break", "continue", "return", "else",
  "struct", "union", "typedef", "static", "enum", "class", "case",
  "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
  "void|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen(void);
char* editorPrompt(char* prompt, void(*callback)(char*, int));

/*** terminal ***/

// Print an error message and exit the program
void die(const char* s) {
    // Clear screen (see editorProcessKeypress())
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Print a descriptive message based on errno
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    // Set terminal attributes to original values, or exit the program on failure
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    // Read current terminal attributes into orig_termios, or exit the program on failure
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    // Defer disabling raw mode until program exit
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
// Turn off misc local flags for this terminal
// using bitwise NOT on the flag
// and then bitwise ANDing the value to the local flags variable
    // Turn off break conditions, carriage return => newline, parity checking, bit stripping, and software data flow control
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // Turn off output post-processing
    raw.c_oflag &= ~(OPOST);
    // Set character size to 8 bits per byte
    raw.c_cflag |= (CS8);
    // Turn off echoing, canonical mode, input processing, and term/suspend signals
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    
    // Set minimum number of bytes of input needed before read() returns
    raw.c_cc[VMIN] = 0;
    // Set maximum amount of time before read() returns to 100 ms
    raw.c_cc[VTIME] = 1;

    // Write flags to attributes, or exit the program on failure
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

// Return keypresses from the terminal
int editorReadKey(void) {
    int nread = 0;
    char c;

    // Run until keypress is detected, then read it to c
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        // Read each character as it is typed, or exit the program on failure
        // Do not treat timeouts as errors
        if (nread == -1 && errno != EAGAIN) {
            die("read");
        }
    }

    // Handle escape characters by reading the next two bytes into buffer seq
    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        // Return correct arrow key based on contents of escape sequence
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                if (seq[2] == '~') {
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
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    // 
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        ++i;
    }

    buf[i] = '\0';
    
    // 
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return -1;
}

int getWindowSize(int* rows, int* cols) {
    struct winsize ws;
    // Easy way: Get the number of rows and cols from the terminal controller
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        // Hard way: Attempt to move the cursor to bottom-right corner, or exit on failure
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        // Return cursor position
        return getCursorPosition(rows, cols);
    } else {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

/*** syntax highlighting ***/

int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

// Update highlighting for all characters
void editorUpdateSyntax(erow* row) {
    // Reallocate memory to account for changes since last highlight pass
    row->hl = realloc(row->hl, row->rsize);
    // Set all characters to normal
    memset(row->hl, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) {
        return;
    }

    char** keywords = E.syntax->keywords;

    // Check for comments
    char* scs = E.syntax->singleline_comment_start;
    char* mcs = E.syntax->multiline_comment_start;
    char* mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);

    // Set highlighting for non-normal characters
    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        // Highlight single-line comments
        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        // Highlight multiline comments
        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        // Highlight strings if enabled for this file type
        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                // Highlight through backslashes if string continues
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) {
                    in_string = 0;
                }
                i++;
                prev_sep = 1;
                continue;
            } else {
                // Highlight single- and double-quoted strings
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        // Highlight numbers if enabled for this file type
        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            // Highlight only digits that are preceded by a separator
            // or are part of a decimal number (including decimal point)
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || 
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        // If the previous character was a separator, compare this word to each
        // word in the list of keywords, and highlight if it is a keyword
        if (prev_sep) {
            int j;
            // Loop through each keyword and compare the correct number of characters
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) {
                    klen--;
                }

                // If it is a keyword, highlight the entire word at once
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

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < E.numrows) {
        editorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

// Return corresponding color for syntax
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: {return 36;}
        case HL_KEYWORD1: {return 33;}
        case HL_KEYWORD2: {return 32;}
        case HL_STRING: {return 35;}
        case HL_NUMBER: {return 31;}
        case HL_MATCH: {return 34;}
        default: {return 37;}
    }
}

// Match the current filename to a matching type in the HLDB
void editorSelectSyntaxHighlight(void) {
    E.syntax = NULL;
    if (E.filename == NULL) {
        return;
    }

    char *ext = strrchr(E.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct editorSyntax* s = &HLDB[j];
        unsigned int i = 0;
        // Loop through filename until extension is found, then compare it to types in HLDB
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;

                int filerow;
                for (filerow = 0; filerow < E.numrows; filerow++) {
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

// Convert a chars index into a render index
int editorRowCxToRx(erow* row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            // Find how many columns we are to the right of the last tab stop,
            // then subtract that from (tab length - 1) to find distance to next tab stop.
            // Add result to rx to position cursor 1 space left of next tab stop
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        // Increment rx regardless of tab char or not
        rx++;
    }

    return rx;
}

// Convert a render index into a chars index
int editorRowRxToCx(erow* row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;

        if(cur_rx > rx) {
            return cx;
        }
    }
    return cx;
}

// Updates contents of the current row
void editorUpdateRow(erow* row) {
    int tabs = 0;
    int j;
    // Get count of tabs (to account for extra memory needed when rendering them)
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    // Render tabs with proper spacing
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

// Append a row to the current array of rows
void editorInsertRow(int at, char* s, size_t len) {
    // Check bounds
    if (at < 0 || at > E.numrows) {
        return;
    }

    // Reallocate memory for the current row
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    // Move current row to next row index
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j < E.numrows; j++) {
        E.row[j].idx++;
    }

    E.row[at].idx = at;

    // Copy the current row char* to the current row in allocated memory
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    E.row[at].hl_open_comment = 0;

    // Update contents of the current row
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

// Free memory for a row
void editorFreeRow(erow* row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editorDelRow(int at) {
    // Check bounds
    if (at < 0 || at >= E.numrows) {
        return;
    }
    // Delete row
    editorFreeRow(&E.row[at]);
    // Move memory for rows after deleted row
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));

    for (int j = at; j < E.numrows - 1; j++) {
        E.row[j].idx--;
    }

    E.numrows--;
    E.dirty++;
}

// Insert a character into a row at an index
void editorRowInsertChar(erow* row, int at, int c) {
    if (at < 0 || at > row->size) {
        at = row->size;
    }
    // Reallocate memory and move characters before and after inserted character
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    // Insert character
    row->chars[at] = c;
    // Update the row in the editor
    editorUpdateRow(row);
    E.dirty++;
}

// Append a string of any size to the end of a row
void editorRowAppendString(erow* row, char* s, size_t len) {
    // Reallocate memory for new size of row
    row->chars = realloc(row->chars, row->size + len + 1);
    // Copy memory of string into row
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    // Append null terminator
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// Delete a character from a row at an index
void editorRowDelChar(erow* row, int at) {
    if (at < 0 || at >= row->size) {
        return;
    }
    // Move row contents before and after character
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    // Shrink row size and update row
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    // Add new row to end of file when needed
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    // Insert character and move cursor to right of character
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// Insert a new line (e.g. with Enter key)
void editorInsertNewLine(void) {
    // Insert new blank row before current line if at beginning of line
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        // Split the current line into two rows
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        // Update pointer to avoid invalidation
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

// Delete a character
void editorDelChar(void) {
    // Return if cursor is beyond the last line
    if (E.cy == E.numrows) {
        return;
    }
    // Return if cursor is at the beginning of the first line
    if (E.cx == 0 && E.cy == 0) {
        return;
    }

    erow* row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    } else {
        // Handle case where cursor is at beginning of line
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file i/o ***/

// Open a file
void editorOpen(char *filename) {
    // Get filename to display in status bar
    free(E.filename);
    E.filename = strdup(filename);

    editorSelectSyntaxHighlight();

    // Open specified file, or exit on failure
    FILE *fp = fopen(filename, "r");
    if (!fp ) {
        die("fopen");
    }

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    // Read each line from the file
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while ( linelen > 0 && (line[linelen - 1] == '\n' ||
                                line[linelen - 1] == '\r')) {
            linelen--;
        }
        // Append row to screen
        editorInsertRow(E.numrows, line, linelen);
    }

    // Free memory and close file
    free(line);
    fclose(fp);
    E.dirty = 0;
}

// Serialize all rows to a single string
char* editorRowsToString(int* buflen) {
    int totlen = 0;
    int j;
    // Add the length of each row to the total length
    for (j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    // Allocate a buffer for the total length of the document
    char* buf = malloc(totlen);
    char* p = buf;
    // Copy all rows into the buffer, using a pointer to track write location
    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

// Save text to a file
void editorSave(void) {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
        editorSelectSyntaxHighlight();
    }

    // Serialize all rows to a single string
    int len;
    char* buf = editorRowsToString(&len);

    // Create a new file if it doesn't exist
    // and open it with standard read/write permissions
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        // Set file size safely in case write fails
        if (ftruncate(fd, len) != -1) {
            // Write serialization buffer to file
            if (write(fd, buf, len) == len) {
                // Close file and free writeout buffer
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    // Free writeout buffer
    free(buf);
    editorSetStatusMessage("Could not save file! Error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char* query, int key) {
    static int last_match = -1;
    static int direction = 1;

    // Infra for saving existing match highlighting
    static int saved_hl_line;
    static char* saved_hl = NULL;

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    // Search forward and backward using arrow keys
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

    // Iterate through rows looking for next/previous match
    if (last_match == -1) {
        direction = 1;
    }
    int current = last_match;
    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }

        erow* row = &E.row[current];
        char* match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            // Save existing highlighting
            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            // Highlight matching text
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(void) {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char* query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

// Append buffer allows update of entire screen at once each refresh
struct abuf {
    char *b;    // pointer to buffer
    int len;    // length of buffer
};

// Append buffer constructor
#define ABUF_INIT {NULL, 0}

// Append a string to an append buffer
// Uses same interface as write(), except writes to the buffer rather than to stdout
void abAppend(struct abuf* ab, const char* s, int len) {
    char *new  = realloc(ab->b, ab->len + len);

    if (new == NULL) {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// Append buffer destructor
void abFree(struct abuf* ab) {
    free(ab->b);
}

/*** input ***/

// Prompt user to input a file name when saving, using status bar
char* editorPrompt(char* prompt, void(*callback)(char*, int)) {
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0 ) {
                editorSetStatusMessage("");
                if (callback) {
                    callback(buf, c);
                }
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

        if (callback) {
            callback(buf, c);
        }
    }
}

// Move cursor using WASD
void editorMoveCursor(int key) {
    // Get current row to do horizontal scrolling checks
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT: {
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                // Moving left at the start of a line moves to the previous line
                // and places the cursor all the way to the right
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        }
        case ARROW_RIGHT: {
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                // Moving right at the end of a line moves to the next line
                // and places the cursor all the way to the left
                E.cy++;
                E.cx = 0;
            }
            break;
        }
        case ARROW_UP: {
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        }
        case ARROW_DOWN: {
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
        }
    }

    // Snaps the cursor to the end of the line (cursor will not move into whitespace)
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

// Handle keypresses
void editorProcessKeypress(void) {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        // Enter key (carriage return symbol)
        case '\r': {
            editorInsertNewLine();
            break;
        }

        // Exit on 'ctrl-q'
        case CTRL_KEY('q'): {
            // Check if program has been modified after last save.
            // If so, require user to input several "quit" commands before exiting
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("Warning! File has unsaved changes. "
                "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }

            // Clear screen (see editorProcessKeypress()) and exit code 0
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        }

        case CTRL_KEY('s'): {
            editorSave();
            break;
        }

        case HOME_KEY: {
            E.cx = 0;
            break;
        }
        case END_KEY: {
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;
        }

        case CTRL_KEY('f'): {
            editorFind();
            break;
        }

        case BACKSPACE: case CTRL_KEY('h'): case DEL_KEY: {
            // Move cursor to the right first if delete key is pressed
            if (c == DEL_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
            break;
        }

        // Move using page up and page down
        case PAGE_UP: case PAGE_DOWN: {
            // Position cursor at either top or bottom of screen
            if (c == PAGE_UP) {
                E.cy = E.rowoff;
            } else if (c == PAGE_DOWN) {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows) {
                    E.cy = E.numrows;
                }
            }
            // Simulate an entire screen's worth of up/down keypresses
            int times = E.screenrows;
            while (times--) {
                editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN); // ternary op!
            }
            break;
        }

        // Move cursor using arrow keys
        case ARROW_LEFT: case ARROW_RIGHT: case ARROW_UP: case ARROW_DOWN: {
            editorMoveCursor(c);
            break;
        }
        
        // Screen refresh
        case CTRL_KEY('l') : {

        }
        // Escape key (and other escape sequences)
        case '\x1b': {
            break;
        }

        // Insert the character corresponding to the key at the current cursor location
        default: {
            editorInsertChar(c);
            break;
        }
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** output ***/

void editorScroll(void) {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // Vertical scrolling
    // Scrolls to cursor if above visible window
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    // Scrolls to cursor if below visible window
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }

    // Horizontal scrolling
    // Scrolls past left edge of screen
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    // Scrolls past right edge of screen
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

// Draw all rows
void editorDrawRows(struct abuf *ab) {
    int y;
    for (y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        // Check whether the current row is part of the text buffer,
        // or whether it is a row after the end of the text buffer
        if (filerow >= E.numrows) {
            // Write a welcome message if the text buffer is empty
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(
                    welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION
                );

                if (welcomelen > E.screencols) {
                    welcomelen = E.screencols;
                }

                // Center the welcome message
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while(padding--) {
                    abAppend( ab, " ", 1);
                }

                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // Display contents of current row
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            // abAppend(ab, &E.row[filerow].render[E.coloff], len); // Append multichar substrings
            // Append substrings char-by-char
            char* c = &E.row[filerow].render[E.coloff];
            unsigned char* hl = &E.row[filerow].hl[E.coloff];
            
            int current_color = -1;
            // For each character, append the corresponding highlight color
            int j;
            for (j = 0; j < len; j++) {
                // Turn control characters into printable characters
                if (iscntrl(c[j])) {
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
                        abAppend(ab, "\x1b[39m", 5);
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

            abAppend(ab, "\x1b[39m", 5);
        }

        // Clear each line before redraw
        abAppend(ab, "\x1b[K", 3);
        // Add a line break at the end of each row
        abAppend(ab, "\r\n", 2);
    }
}

// Draw status bar on 2nd-last row of screen
void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    // Print status bar content on left side of screen
    int len = snprintf(
        // Print first 20 characters of filename and number of rows
        status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        // Print indicator if file has been modified
        E.dirty ? "(modified)" : "");
    // Print current line number on right side of screen
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d", 
        E.syntax ? E.syntax->filetype : "no ft",
        E.cy + 1, E.numrows);
    // Cap length at the number of columns on screen
    if (len > E.screencols) {
        len = E.screencols;
    }
    // Append accumulated text to status bar
    abAppend(ab, status, len);
    // Print remaining characters in status bar as spaces
    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

// Draw message bar on last row of screen
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) {
        msglen = E.screencols;
    }
    // Only display message if it is less than 5 seconds old
    if (msglen && time(NULL) - E.statusmsg_time < 5) {
        abAppend(ab, E.statusmsg, msglen);
    }
}

// Clear the screen and draw all rows
void editorRefreshScreen(void) {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // Hide cursor
    abAppend(&ab, "\x1b[?25l", 6);
    // Position cursor at top-left corner
    abAppend(&ab, "\x1b[H", 3);

    // Draw rows on screen
    editorDrawRows(&ab);
    // Draw status bar at bottom of screen
    editorDrawStatusBar(&ab);
    // Draw message bar at bottom of screen
    editorDrawMessageBar(&ab);

    // Position cursor at coordinates stored in editor state E
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
            (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // Write entire append buffer to screen at once
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// Set status bar message (variadic function)
void editorSetStatusMessage(const char* fmt, ...) {
    // Initialize a variadic list
    va_list ap;

    // Print the messages in the list
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);

    E.statusmsg_time = time(NULL);
}

// Initialize the editor window
void initEditor(void) {
    // Position cursor at top-left corner
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;

    E.filename = NULL;
    E.dirty = 0;

    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    E.syntax = NULL;

    // Get window size, or exit on failure
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
    // Prevent drawing 2 rows at the bottom of the screen
    // to reserve space for status bar and message bar
    E.screenrows -= 2;
}

/*** init ***/
int main(int argc, char* argv[]) {

    enableRawMode();
    initEditor();

    // Open file if specified
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}

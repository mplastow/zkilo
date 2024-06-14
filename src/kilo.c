/*** includes ***/

// Feature test macro
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

// bitwise AND Ctrl-key with a given character
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
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

/*** data ***/

// Data type for storing a row of text
typedef struct erow {
    int size;
    int rsize;
    char* chars;
    char* render;
} erow;

struct editorConfig {
    int cx, cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;

    int numrows;
    erow* row;  // array of rows in editable text

    struct termios orig_termios;
};

// Global container for editor state
struct editorConfig E;

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

/*** row operations ***/

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
}

void editorAppendRow(char* s, size_t len) {
    // Reallocate memory for the current row
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

    // Copy the current row char* to the current row in allocated memory
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;

    // Update contents of the current row
    editorUpdateRow(&E.row[at]);

    E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
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
        editorAppendRow(line, linelen);
    }

    // Free memory and close file
    free(line);
    fclose(fp);
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
    int c = editorReadKey();

    // Exit on 'ctrl-q'
    switch (c) {
        case CTRL_KEY('q'): {
        // Clear screen (see editorProcessKeypress()) and exit code 0
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        }

        case HOME_KEY: {
            E.cx = 0;
            break;
        }
        case END_KEY: {
            E.cx = E.screencols - 1;
            break;
        }

        // Move using page up and page down
        case PAGE_UP: case PAGE_DOWN: {
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
    }
}

/*** output ***/

void editorScroll(void) {
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
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    // Scrolls past right edge of screen
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
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
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        // Clear each line before redraw
        abAppend(ab, "\x1b[K", 3);
        // Add a line break at the end of all rows except the last
        if (y < E.screenrows - 1) {
            abAppend(ab, "\r\n", 2);
        }
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

    // Draw rows of tildes
    editorDrawRows(&ab);

    // Position cursor at coordinates stored in editor state E
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
            (E.cy - E.rowoff) + 1, (E.cx - E.coloff) + 1);
    abAppend(&ab, buf, strlen(buf));

    // Show cursor
    abAppend(&ab, "\x1b[?25h", 6);

    // Write entire append buffer to screen at once
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// Initialize the editor window
void initEditor(void) {
    // Position cursor at top-left corner
    E.cx = 0;
    E.cy = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;

    // Get window size, or exit on failure
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }
}

/*** init ***/
int main(int argc, char* argv[]) {

    enableRawMode();
    initEditor();

    // Open file if specified
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}

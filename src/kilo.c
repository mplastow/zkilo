#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

// Prints an error message and exits the program
void die(const char* s) {
    // Print a descriptive message based on errno
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    // Set terminal attributes to original values, or exit the program on failure
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    // Read current terminal attributes into orig_termios, or exit the program on failure
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    }
    // Defer disabling raw mode until program exit
    atexit(disableRawMode);

    struct termios raw = orig_termios;
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

int main(void) {

    enableRawMode();

    // Run until break
    while (1) {
        char c = '\0';
        // Read each character as it is typed, or exit the program on failure
        // Do not treat timeouts as errors
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
            die("read");
        }

        // Decide what to display for each character
        if (iscntrl(c)) {
            // Skips displaying control characters, but prints the byte
            printf("%d\r\n", c);
        } else {
            // Displays any other character and its byte
            printf("%d ('%c')\r\n", c, c);
        }

        // Break on 'q'
        if (c == 'q') {
            break;
        }
    }

    return 0;
}

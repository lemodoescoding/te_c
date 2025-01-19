#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

// --- Data ---
struct editorConfig {
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

// --- TERMINAL ---

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);
  perror(s);

  exit(1);
}

void disableRawMode() {
  // tcsetattr and tcgetattr returns -1 on failure
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr"); // get terminal attr

  atexit(disableRawMode); // when exits, disable raw mode

  struct termios raw = E.orig_termios;

  raw.c_iflag &=
      ~(BRKINT | ICRNL | INPCK | ISTRIP |
        IXON); // disable Ctrl-S, Ctrl-Q, i = input, disable carriage return
  raw.c_oflag &= ~(OPOST); // disabling carriage return for outputting
  raw.c_cflag |= (CS8);    // bit mask, and not a flag
  raw.c_lflag &= ~(
      ECHO | ICANON | IEXTEN |
      ISIG); // disable sigterm, sigint, Ctrl-V, echoing to terminal, l = local

  // cc is for control character
  raw.c_cc[VMIN] = 0; // set minimum number of bytes of input needed before
                      // read() can return, 0 to read as soon there is any input
  raw.c_cc[VTIME] = 1; // maximum time to wait before read() returns

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr"); // set the attr
}

char editorReadKey() {
  int nread;

  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) !=
         1) { // detects incoming keypress
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    return -1;

  // what is the loop does is, the above if condition if succeed, it will return
  // an escape character followed by the the editor or the terminal size! and it
  // ends with R character, 24;80R
  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1)
      break;
    if (buf[i] == 'R')
      break;

    i++;
  }

  buf[i] = '\0'; // set the last byte to NULL

  if (buf[0] != '\x1b' || buf[1] != '[')
    return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    return -1;

  return -1;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // ioctl defined on sys/ioctl.h
  // ioctl on success will replace the ws_row and ws_col in the winsize struct
  // if failed it will returns -1
  //
  // ioctl is not guaranteed to work on all machine, therefore we must think and
  // implement the hard way to get the user terminal window size using escape
  // keys
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
      return -1;

    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

// --- INPUT ---

void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
}

// --- APPEND BUFFER ---
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) { free(ab->b); }

// --- OUTPUT ---
void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows;
       y++) { // E.screenrows will have the appropriate screen rows size after
              // succeeding get the values
    write(STDOUT_FILENO, "~", 1); // change this line

    // the last line of the terminal screen seems not to print a tilde, this is
    // a bug on our code when it reaches the last line, it prints the \r\n and
    // causes the terminal to scroll in order to make room for the new blank
    // line
    if (y < E.screenrows - 1) {
      write(STDOUT_FILENO, "\r\n", 2);
    }
  }
}
void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4); // clears the screen
  write(STDOUT_FILENO, "\x1b[H", 3);  // moves the cursor to the top

  editorDrawRows();

  write(STDOUT_FILENO, "\x1b[H", 3);
}

// --- INIT ---

void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main() {
  enableRawMode();
  initEditor(); // searches the rows and cols for the editor

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <asm-generic/ioctls.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

#define KILO_VERSION "0.0.1"

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

// --- Data ---
typedef struct erow { // for storing size of the file and the chars in them
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx, cy;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows; //
  erow *row;   // store multiple erow structs
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

int editorReadKey() {
  int nread;

  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) !=
         1) { // detects incoming keypress
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c ==
      '\x1b') { // for detecting escape key sequences (arrow and other things)
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') { // checks if after the escape character is [ character
      if (seq[1] >= '0' &&
          seq[1] <= '9') { // options when the escape key seq is page_up or down
                           // and home and end key
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';

        if (seq[2] == '~') { // detects for h/k or pu/pd
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY; // delete key -> \x1b[3~
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else { // options when the escape key seq is arrow
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') { // handles the home and end key, since different
                                // os have different escape key seq
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
  } else {

    return c;
  }
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

// --- ROW OPERATIONS ---
void editorAppendRow(char *s, size_t len) {
  E.row = realloc(
      E.row,
      sizeof(erow) * (E.numrows + 1)); // reallocate the memory to store new row
  // take the size of the erow and multiply by the number on numrows + 1

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);

  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.numrows++;
}

// --- FILE I/O ---

void editorOpen(char *filename) {
  FILE *fp = fopen(
      filename,
      "r"); // open the file based on the argument passed when running the app

  if (!fp)
    die("fopen");

  char *line = NULL;

  size_t linecap = 0; // line capacity, to know how much memory is allocated
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) !=
         -1) { // continously read each line of the provided file argument until
               // it reaches the EOF
    // to read line doesnt matter the length until it reaches \n

    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;

    editorAppendRow(line, linelen);
  }

  free(line);
  fclose(fp);
}

// --- INPUT ---

void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  // to know what is the len of current line

  // to move the cursor inside the terminal window, and also checks whether
  // the cursor has reaches the end of each side of the window
  switch (key) {
  case ARROW_LEFT:
    if (E.cx != 0)
      E.cx--;
    else if (E.cy >
             0) { // allow cursor when hit the left screen to continue upward
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size) { // preventing user to able to move the cursor
                                   // outside of the line
      E.cx++;
    } else if (row &&
               E.cx == row->size) { // makes the cursor put to the beginning of
                                    // new line when reaches the end
      E.cy++;
      E.cx = 0;
    }
    break;
  case ARROW_UP:
    if (E.cy != 0)
      E.cy--;
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows)
      E.cy++;
    break;
  }

  row = (E.cy >= E.numrows) ? NULL
                            : &E.row[E.cy]; // the next line or previous line

  int rowlen = row ? row->size : 0;

  if (E.cx > rowlen) {
    E.cx = rowlen; // forces the cursor to stay in the last character on each
                   // line after doing cursor movement (up or down)
  }
}

void editorProcessKeypress() {
  // handles what key is pressed
  int c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    E.cx = E.screencols - 1;
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    int times = E.screenrows;
    while (times--)
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
  } break;

  case ARROW_DOWN: // for navigating or cursor movement
  case ARROW_UP:
  case ARROW_LEFT:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  }
}

// --- APPEND BUFFER ---
// append buffer to reduce the consumption of write function
// and also limitting bugs to occur when using write func
// abuf is a dynamically string, C doesnt have this, we have to do it manually

struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s,
              int len) { // append the new string with is appropriate length to
                         // be added to the abuf
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL)
    return;

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct abuf *ab) {
  free(ab->b);
} // dont forget to free the fucking memory used by the abuf, since it uses
  // realloc

// --- OUTPUT ---

void editorScroll() {
  // to check if the cursor has moved outside of the visible window, and adjust
  // the cursor to be inside the visible window or terminal window
  if (E.cy < E.rowoff) {
    // checks if the cursor is above the visible window
    E.rowoff = E.cy;
  }

  if (E.cy >= E.rowoff + E.screenrows) {
    // checks if the cursor is past the bottom of the visible window
    E.rowoff = E.cy - E.screenrows + 1;
  }

  // for the horizontal part, pretty much the same as the vertical one above
  if (E.cx < E.coloff) {
    E.rowoff = E.cx;
  }

  if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows;
       y++) { // E.screenrows will have the appropriate screen rows size after
              // succeeding get the values
    // write(STDOUT_FILENO, "~", 1); // change this line

    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      if (y == E.screenrows / 3 &&
          E.numrows == 0) { // welcome screen only show when there is no
                            // arguments included when the program is called
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "TE editor -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols)
          welcomelen = E.screencols;

        int padding = (E.screencols - welcomelen) / 2;

        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }

        while (padding-- >= 0) { // makes the welcome screen text centered
          abAppend(ab, " ", 1);
        }

        abAppend(ab, welcome, welcomelen); // prints the welcome screen text
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len =
          E.row[filerow].size -
          E.coloff; // draws the input text to the buffer ab on
                    // each line of erow, now coloff serve as an index of the
                    // chars each time each row is displayed to the screen

      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].chars[E.coloff],
               len); // writes whatever in E.row.chars to the ab
    }

    abAppend(ab, "\x1b[K", 3); // clean the remaining length of the line

    // the last line of the terminal screen seems not to print a tilde, this is
    // a bug on our code when it reaches the last line, it prints the \r\n and
    // causes the terminal to scroll in order to make room for the new blank
    // line
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
      /* write(STDOUT_FILENO, "\r\n", 2); */
    }
  }
}
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  /* abAppend(&ab, "\x1b[2J", 4); */
  abAppend(&ab, "\x1b[H", 3);

  // write(STDOUT_FILENO, "\x1b[2J", 4); // clears the screen
  // write(STDOUT_FILENO, "\x1b[H", 3);  // moves the cursor to the top

  editorDrawRows(&ab);

  // write(STDOUT_FILENO, "\x1b[H", 3);
  // E.cy now only refers the cursor position within the text file, not the
  // window
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

// --- INIT ---

void initEditor() {
  // sets the initial location of the cursor to be in (0,0) -> leftmost and
  // topmost of the screen
  E.cx = 0;
  E.cy = 0;

  E.rowoff = 0; // set offset of scrolling to 0
  E.coloff = 0; // same as rowoff, it's now column
  E.numrows = 0;
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor(); // searches the rows and cols for the editor

  if (argc >= 2) { // if not te.c not called with argument, editorOpen will not
                   // be called
    editorOpen(argv[1]);
  }

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

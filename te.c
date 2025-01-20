#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// --- DEFINES ---

#define CTRL_KEY(k) ((k) & 0x1f)

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 4
#define KILO_QUIT_TIMES 3

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

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

// --- Data ---

struct editorSyntax {
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  char *multiline_comment_start;
  char *multiline_comment_end;
  int flags;
};

typedef struct erow { // for storing size of the file and the chars in them
  int idx;
  int size;
  int rsize; // size of the contents of render
  char *chars;
  char *render;      // contains actual character to draw on the screen
  unsigned char *hl; // integers in range 0 - 255, an array of unsigned char
  int hl_open_comment;
} erow;

struct editorConfig {
  int cx, cy;
  int rx; // cx is for index for chars, rx for render field
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows; //
  erow *row;   // store multiple erow structs
  int dirty;   // to know if the current buffer is already changed and saved to
               // disk
  char *filename; // stores the filename being loaded
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  struct termios orig_termios;
};

struct editorConfig E;

// --- FILETYPES ---
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

char *C_HL_keywords[] = {"switch",    "if",      "while",   "for",    "break",
                         "continue",  "return",  "else",    "struct", "union",
                         "typedef",   "static",  "enum",    "class",  "case",

                         "int|",      "long|",   "double|", "float|", "char |",
                         "unsigned|", "signed|", "void|",   NULL};

// HLDB -> highlight db
struct editorSyntax HLDB[] = {{"c", C_HL_extensions, C_HL_keywords, "//", "/*",
                               "*/",
                               HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS}};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

// --- PROTOTYPES ---

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

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
          seq[1] <= '9') { // options when the escape key seq is page_up or
                           // down and home and end key
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
    } else if (seq[0] == 'O') { // handles the home and end key, since
                                // different os have different escape key seq
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

// --- SYNTAX HIGHLIGHTING ---

int is_separator(int c) {
  // looks for separator, using isspace, null or strchr
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c);
}

void editorUpdateSyntax(erow *row) {
  row->hl = realloc(
      row->hl, row->rsize); // allocate hl memory block to be the same as rsize
  memset(row->hl, HL_NORMAL,
         row->rsize); // set all character to be HL_NORMAL by default

  if (E.syntax == NULL) // if no filetype is set, return immediately
                        // by this point, the entire line is set to HL_NORMAL
    return;

  char **keywords = E.syntax->keywords;

  char *scs = E.syntax->singleline_comment_start;
  char *mcs = E.syntax->multiline_comment_start; // multiline_comment_start
  char *mce = E.syntax->multiline_comment_end;   // multiline_comment_end
  // scs for singleline_comment_start, scs_len is 0 when scs is NULL

  int scs_len = scs ? strlen(scs) : 0;
  int mcs_len = mcs ? strlen(mcs) : 0;
  int mce_len = mce ? strlen(mce) : 0;

  int prev_sep = 1; // previous_separator, defaults to 1 -> true,
  // consider the beginning of the line to be a separator

  int in_string = 0; // tracks if the current syntax is in string or not
  int in_comment = (row->idx > 0 && E.row[row->idx - 1].hl_open_comment);
  // checks if current is inside a comment, by checking previous row
  // hl_open_comment is set to 1 or not

  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];

    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
    // prev_hl set to the hl type of the previous character

    if (scs_len && !in_string &&
        !in_comment) { // checks if not inside a string and inside comment block
      // checks the character is the start of the sng-line comment
      if (!strncmp(&row->render[i], scs, scs_len)) {
        // set the rest of the row to be HL_COMMENT
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    if (mcs_len && mce_len && !in_string) {
      if (in_comment) { // checks if in a comment block
        row->hl[i] = HL_MLCOMMENT;

        if (!strncmp(&row->render[i], mce, mce_len)) {
          // if on a comment block, set in_comment = 0
          memset(&row->hl[i], HL_MLCOMMENT, mce_len);

          i += mce_len;
          in_comment = 0;

          prev_sep = 1;
          continue;
        } else {
          i++;
          continue;
        }
      } else if (!strncmp(&row->render[i], mcs,
                          mcs_len)) { // detects the beginning of comment block
        memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
        i += mcs_len;

        in_comment = 1;
        continue;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      // checks if the flags is also set to highlight string
      if (in_string) {
        // if current char is in string, then hl[i] = HL_STRING
        row->hl[i] = HL_STRING;

        if (c == '\\' && i + 1 < row->rsize) {
          // checks the occurance of \' and \"
          row->hl[i + 1] = HL_STRING;
          i += 2;

          // skip the 2 chars and continue to the index += 2
          continue;
        }

        if (c == in_string)
          in_string = 0; // if current char c is the same as the beginning of
                         // the string, set in_string = 0
        i++;

        prev_sep = 1;
        continue;
      } else {
        if (c == '"' ||
            c == '\'') { // checks if current char is dbl-quote or sngl-quot
          in_string = c;

          // set in_string to c;
          // and HL_STRING current hl[i]
          row->hl[i] = HL_STRING;

          i++;
          continue;
        }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(row->render[i]) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        // to highlight a digit, it is now required that the previous character
        // needs to be either separator or HL_NUMBER
        //
        // additional conditining to highlight decimal point aswell if it is
        // between a number

        row->hl[i] = HL_NUMBER; // set the hl to be a number
        i++;                    // moves to the next character

        prev_sep = 0; // set this to 0, to indicate we are in the middle of
                      // highlighting something,
        // and continue the loop
        continue;
      }
    }

    if (prev_sep) { // make sure there is a separator before the char
      int j;
      for (j = 0; keywords[j]; j++) {
        int klen =
            strlen(keywords[j]); // checks each keyword available to highlight
        int kw2 = keywords[j][klen - 1] ==
                  '|'; // checks if the keyword is the secondary

        if (kw2)
          klen--; // reduce the klen if kw2 is true

        if (!strncmp(&row->render[i], keywords[j], klen) &&
            is_separator(row->render[i + klen])) {
          // checks if the word is separated from strings
          // compare the string from i to i + klen is actually the keyword
          //
          // set the highlight based on what keyword 1 or keyword 2 group
          memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);

          i += klen; // jumps the i to i + klen
          break;
        }
      }

      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    // if the current character is not highlighted, then set prev_sep
    // according to whatever the current character is a separator

    prev_sep = is_separator(c);
    i++;
  }

  // updates syntax if there is multiline_comment_start
  int changed = (row->hl_open_comment != in_comment);
  row->hl_open_comment = in_comment;

  if (changed &&
      row->idx + 1 <
          E.numrows) // if hl_open_comment changed, call editorUpdateSyntax
    editorUpdateSyntax(&E.row[row->idx + 1]);
}

int editorSyntaxToColor(int hl) {
  switch (hl) {
  case HL_COMMENT:
  case HL_MLCOMMENT:
    return 36; // gray -> 90, cyan -> 36
  case HL_KEYWORD1:
    return 33;
  case HL_KEYWORD2:
    return 32;
  case HL_STRING:
    return 35; // magenta
  case HL_NUMBER:
    return 31; // red
  case HL_MATCH:
    return 34; // i believe this is blue
  default:
    return 37;
  }
}

void editorSelectSyntaxHighlight() {
  E.syntax = NULL;

  if (E.filename == NULL)
    return;

  char *ext =
      strrchr(E.filename, '.'); // locates the last occurance of char . in the
                                // filename if no ext returns NULL

  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    // try to match what is the syntax on the HLDB
    struct editorSyntax *s = &HLDB[j];

    unsigned int i = 0;
    while (s->filematch[i]) {
      int is_ext = (s->filematch[i][0] == '.');

      // if the pattern starts with . then it is a file ext pattern
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
          (!is_ext && strstr(E.filename, s->filematch[i]))) {
        // compare the ext and the filematch[i]
        E.syntax = s;

        // this for rehighlighting the entire line after setting E.syntax
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

// --- ROW OPERATIONS ---

int editorRowCxToRx(erow *row, int cx) {
  // this to deal with movement of the cursor when it find a tab character
  int rx = 0;

  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) -
            (rx % KILO_TAB_STOP); // to find how many columns are to the right
                                  // of the last tab stop

    rx++;
  }

  return rx;
}

int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  int cx;

  // this is basically the inverse of editorRowCxToRx function

  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t') // handles when upon meets with tab char
      cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    cur_rx++;

    if (cur_rx > rx)
      return cx;
  }

  return cx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size;
       j++) { // counts how many tabs are there within a line
    if (row->chars[j] == '\t')
      tabs++;
  }

  free(row->render);

  row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) +
                       1); // sets render size to acommodate tabs

  int idx = 0; // contains the number of chars copied to row->render
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') { // when tabs are present
      row->render[idx++] = ' ';
      while ((idx % KILO_TAB_STOP) != 0) // iterate until gets to a tab stop,
                                         // which is a column divisible by 8
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows)
    return;

  E.row = realloc(
      E.row,
      sizeof(erow) * (E.numrows + 1)); // reallocate the memory to store new row
  // take the size of the erow and multiply by the number on numrows + 1

  memmove(&E.row[at + 1], &E.row[at],
          sizeof(erow) *
              (E.numrows -
               at)); // make room at the specified index fo the next new row

  for (int j = at + 1; j <= E.numrows; j++)
    E.row[j].idx++; // updates row index after current row

  E.row[at].idx = at; // set the row index to at variable

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);

  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;

  E.row[at].hl_open_comment = 0;

  editorUpdateRow(&E.row[at]); // for rendering tabs

  E.numrows++;
  E.dirty++; // increase the dirty value
}

void editorFreeRow(erow *row) {
  free(row->render); // free the memory by the row we want to delete
  free(row->chars);
  free(row->hl);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return; // validate the index if it's valid to delete

  editorFreeRow(&E.row[at]);
  memmove(
      &E.row[at], &E.row[at + 1],
      sizeof(erow) *
          (E.numrows - at -
           1)); // overwrite the deleted row struct with the rest of the rows

  for (int j = at; j < E.numrows - 1; j++)
    E.row[j].idx--; // decrease the row index below current row

  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size)
    at = row->size; // at is the index we want to insert the character
  // at can go beyond 0 or beyond the row len limit

  row->chars =
      realloc(row->chars,
              row->size + 2); // allocate 2 byte for the new char and null byte
  memmove(&row->chars[at + 1], &row->chars[at],
          row->size - at + 1); // make new room for the new chars
  row->size++;                 // this is self-explanatory

  row->chars[at] = c;   // assign the char on the appropriate position -> at
  editorUpdateRow(row); // -> update the row with the new content

  E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(
      row->chars,
      row->size + len +
          1); // reallocate the new size of the row, + 1 is for the null byte

  memcpy(&row->chars[row->size], s, len); // copy the memory

  row->size += len; // update the row size by len
  row->chars[row->size] = '\0';

  editorUpdateRow(row);

  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size)
    return;

  memmove(&row->chars[at], &row->chars[at + 1],
          row->size - at); // memmove to overwrite the deleted character with
                           // the characters that come after it
  row->size--;

  editorUpdateRow(row);
  E.dirty++;
}

// --- EDITOR OPERATIONS ---

void editorInsertChar(int c) {
  if (E.cy == E.numrows) { // when the cursor is on the very bottom
    editorInsertRow(E.numrows, "",
                    0); // append a new row on there before inserting anything
  }

  editorRowInsertChar(&E.row[E.cy], E.cx, c); // insert the char
  E.cx++;
}

void editorInsertNewline() {
  // inserts a new blank line row before the line
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    // split the line into two rows, separated by what the cursor is pointing
    erow *row = &E.row[E.cy];

    // insert a new row and append string after the cursor when we hit enter
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0'; // sets the last index to be null

    // this func is always called whenever we have update in the rows
    editorUpdateRow(row);
  }

  E.cy++;
  E.cx = 0;
}

void editorDelChar() {
  if (E.cy == E.numrows)
    return; // if the cursor are past the the end of the file, nothing to be
            // deleted

  if (E.cx == 0 && E.cy == 0)
    return;

  erow *row = &E.row[E.cy]; // grab the pointer of the current row erow

  if (E.cx > 0) {
    editorRowDelChar(
        row, E.cx - 1); // deletes one character and moves the cursor back by 1
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars,
                          row->size); // append the string on the current line
                                      // or row to the previous
    editorDelRow(E.cy);               // deletes the current row / line

    E.cy--;
  }
}

// --- FILE I/O ---

void *editorRowsToString(int *buflen) {
  int totlen = 0; // store the total length of the entire text
  int j;

  for (j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1; // adding one to each line for return char
  }

  *buflen = totlen;

  char *buf = malloc(totlen); // allocate the required memory
  char *p = buf;              // pointer to buf

  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars,
           E.row[j].size); // copy the entire line to the p -> buf
    p += E.row[j].size;    // move the pointer in size with the row
    *p = '\n';             // add return char at the end
    p++;                   // move the pointer by 1
  }

  return buf; // returns the pointer of buf, expected to free the memory by the
              // caller
}

void editorOpen(char *filename) {
  free(E.filename);

  E.filename = strdup(filename); // duplicates filename to E.filename

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(
      filename,
      "r"); // open the file based on the argument passed when running the app

  if (!fp)
    die("fopen");

  char *line = NULL;

  size_t linecap = 0; // line capacity, to know how much memory is allocated
  ssize_t linelen;

  while ((linelen = getline(&line, &linecap, fp)) !=
         -1) { // continously read each line of the provided file argument
               // until it reaches the EOF
    // to read line doesnt matter the length until it reaches \n

    while (linelen > 0 &&
           (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
      linelen--;

    editorInsertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);

  E.dirty = 0; // sets E.dirty to 0 after opening a file
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s", NULL);

    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }

    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT,
                0644); // create the file based on the name of the filename
  // O_RDWR -> reading and writing
  // O_CREAT -> create new file if it doesnt already exist, 0644 is the argument
  // for fie permission, just like the chmod file/dir permission

  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      // sets the file size to len, and also truncate any data
      // that is larger than len
      // the normal way to overwrite a file is to pass O_TRUNC flag to open()
      // func
      // -> trucates the file completely truncating the file len first is
      // actually safer, since if the write operation below us fails, we still
      // have the remaining data until the new len, so we don't actually lose
      // all the stuff.
      //
      // most advanced editors will write to a new temp file, then rename the
      // file to the actual file that user wants to overwrite, and then
      // carefully check for errors through the whole process

      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);

        E.dirty = 0; // sets the dirty value to 0 after saving the file

        editorSetStatusMessage("%d bytes written to disk",
                               len); // send message to messagebuf
        return;
      } // write the bytes
    }
    close(fd); // close the open
  }

  free(buf);
  editorSetStatusMessage(
      "Can't save! I/O error: %s",
      strerror(errno)); // strerror returns the human readable string for the
                        // provided error code
}

// --- FIND ---

void editorFindCallback(char *query, int key) {
  static int last_match = -1;
  static int direction = 1;

  static int saved_hl_line;     // save the previous line highlighted
  static char *saved_hl = NULL; // save the original hl line

  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl); // free the fucking memory, after hl is restored

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

  if (last_match == -1)
    direction = 1;

  int current =
      last_match; // current is the index of the current row we are searching
  // if there was a last match, it starts on the line after,
  // if there isnt is starts at the top of the file

  int i;
  for (i = 0; i < E.numrows; i++) {
    current += direction;
    if (current == -1)
      current = E.numrows - 1; // causes current to go from the end of the file
                               // back to the beginning
    else if (current == E.numrows)
      current = 0;

    erow *row = &E.row[current];

    char *match = strstr(row->render, query);
    // query search using strstr function
    // the rest is self-explanatory code

    if (match) {
      last_match = current; // updates last_match to current
      E.cy = current;
      E.cx = editorRowRxToCx(
          row,
          match - row->render); // converts the rx to cx, since cx is referring
                                // to the position of char, not on the screen
      E.rowoff = E.numrows;

      saved_hl_line = current;       // set the current hl to saved_hl_line
      saved_hl = malloc(row->rsize); // make the size fit for the line
      memcpy(saved_hl, row->hl,
             row->rsize); // copy the entire content of the line
      memset(&row->hl[match - row->render], HL_MATCH,
             strlen(query)); // highlight the search match
      // match - row->render is the index of the matched search keyword
      break;
    }
  }
}

void editorFind() {
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  char *query =
      editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

  if (query)
    free(query);
  else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

// --- INPUT ---

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  // prompt is expected to be a format string containing a %s user input
  size_t bufsize = 128;        // user input will be stored in this buf
  char *buf = malloc(bufsize); // here on buf

  size_t buflen = 0;
  buf[0] = '\0'; // initialize the buf string to empty string, despite there is
                 // null char inside

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey(); // read the key user press after
    if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback)
        callback(buf, c);

      free(buf);
      return NULL;
    } else if (c ==
               '\r') { // if the key pressed is enter, clear the status message
      if (buflen != 0) { // and if the buf is not empty
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);

        return buf;
      }
    } else if (!iscntrl(c) && c < 128) { // if c is not a control or special
                                         // key (key id is less than 128)
      if (buflen == bufsize - 1) { // if the buflen reached the maximum capacity
        bufsize *= 2;              // double the size of the bufsize
        buf = realloc(buf, bufsize); // and reallocate them
      }

      buf[buflen++] = c;
      buf[buflen] = '\0';
    } else if (c == BACKSPACE || c == DEL_KEY || c == CTRL_KEY('h')) {
      // when the key pressed is the bs, del, or ctrl-h

      if (buflen != 0)
        buf[--buflen] = '\0';
    }

    if (callback)
      callback(buf, c);
  }
}

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
    if (row && E.cx < row->size) { // preventing user to able to move the
                                   // cursor outside of the line
      E.cx++;
    } else if (row &&
               E.cx == row->size) { // makes the cursor put to the beginning
                                    // of new line when reaches the end
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

  static int quit_times =
      KILO_QUIT_TIMES; // keep track how many Ctrl-Q has been pressed

  switch (c) {
  case '\r':
    // when enter key is pressed
    editorInsertNewline();
    break;

  case CTRL_KEY('q'):

    if (E.dirty && quit_times > 0) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quit_times);
      quit_times--;

      return;
    }

    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;

  case CTRL_KEY('s'):

    editorSave();
    break;

  case HOME_KEY:
    E.cx = 0;
    break;

  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size; // ensures to move to the end of the line
                               // defined by the row cy size
    break;

  case CTRL_KEY('f'):
    editorFind();
    break;

  case BACKSPACE:
  case CTRL_KEY('h'):
  case DEL_KEY:
    if (c == DEL_KEY) // when the pressed key is DEL_KEY, move the cursor to
                      // the right by 1
      editorMoveCursor(ARROW_RIGHT);

    editorDelChar();
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c ==
        PAGE_UP) { // positions the cursor at the top or bottom of the screen
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cy = E.rowoff + E.screenrows - 1;

      if (E.cy > E.numrows)
        E.cy = E.numrows;
    }

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

  case CTRL_KEY('l'):
  case '\x1b':
    break;

  default:
    editorInsertChar(c); // insert the new char to the row
    break;
  }

  quit_times = KILO_QUIT_TIMES;
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
  E.rx = 0;

  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy],
                           E.cx); // calculates the right value for E.rx
  }

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
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
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
          E.row[filerow].rsize -
          E.coloff; // draws the input text to the buffer ab on
                    // each line of erow, now coloff serve as an index of the
                    // chars each time each row is displayed to the screen

      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      char *c = &E.row[filerow].render[E.coloff];

      unsigned char *hl =
          &E.row[filerow].hl[E.coloff]; // grab the hl index based on the coloff

      int current_color = -1; // -1 for default color

      int j;
      for (j = 0; j < len; j++) {
        if (iscntrl(c[j])) { // checks if the current char is control char
          char sym = (c[j] <= 26)
                         ? '@' + c[j]
                         : '?'; // if the char <= 26 print @_ otherwise ?

          abAppend(ab, "\x1b[7m", 4); // invert the color
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3); // put back to normal

          if (current_color !=
              -1) { // if the color not the normal ones, set it back
                    // since \x1b[m reset all color or highlight
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
            abAppend(ab, buf, clen);
          }

        } else if (hl[j] == HL_NORMAL) { // if its HL_NORMAL, use \x1b[39m
          if (current_color !=
              -1) { // and if the current_color is not -1, set it back
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }

          abAppend(ab, &c[j], 1);
        } else { // otherwise use the appropriate color
          int color = editorSyntaxToColor(hl[j]);

          if (color != current_color) { // update the color
            current_color = color;

            char buf[16];

            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);

            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }

      abAppend(ab, "\x1b[39m", 5); // make the text color is reset to default
    }

    abAppend(ab, "\x1b[K", 3); // clean the remaining length of the line

    // the last line of the terminal screen seems not to print a tilde, this is
    // a bug on our code when it reaches the last line, it prints the \r\n and
    // causes the terminal to scroll in order to make room for the new blank
    // line

    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);

  char status[80];
  char rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");

  int rlen =
      snprintf(rstatus, sizeof(rstatus), "%s | %d,%d",
               E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.rx + 1);

  if (len > E.screencols)
    len = E.screencols - 1;

  abAppend(ab, status, len);

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

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3); // clears the line

  int msglen = strlen(E.statusmsg);

  if (msglen > E.screencols)
    msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  // abAppend(&ab, "\x1b[2J", 4);
  abAppend(&ab, "\x1b[H", 3);

  // write(STDOUT_FILENO, "\x1b[2J", 4); // clears the screen
  // write(STDOUT_FILENO, "\x1b[H", 3);  // moves the cursor to the top

  editorDrawRows(&ab);

  editorDrawStatusBar(&ab);

  editorDrawMessageBar(&ab);

  // write(STDOUT_FILENO, "\x1b[H", 3);
  // E.cy now only refers the cursor position within the text file, not the
  // window
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);

  va_end(ap);

  E.statusmsg_time = time(NULL);
}

// --- INIT ---

void initEditor() {
  // sets the initial location of the cursor to be in (0,0) -> leftmost and
  // topmost of the screen

  E.cx = 0;
  E.cy = 0;

  E.rx = 0;

  E.rowoff = 0; // set offset of scrolling to 0
  E.coloff = 0; // same as rowoff, it's now column
  E.numrows = 0;
  E.row = NULL;

  E.dirty = 0;

  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  E.syntax = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");

  E.screenrows -= 2; // to make room for the status bar and status message
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor(); // searches the rows and cols for the editor

  if (argc >= 2) { // if not te.c not called with argument, editorOpen will not
                   // be called
    editorOpen(argv[1]);
  }

  editorSetStatusMessage(
      "HELP: Ctrl-Q to quit | Ctrl-S to save | Ctrl-F to find");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}

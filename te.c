#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

void die(const char *s) {
  perror(s);

  exit(1);
}

void disableRawMode() {
  // tcsetattr and tcgetattr returns -1 on failure
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcgetattr"); // get terminal attr

  atexit(disableRawMode); // when exits, disable raw mode

  struct termios raw = orig_termios;

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

int main() {
  enableRawMode();

  while (1) {
    char c = '\0';

    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");

    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')", c, c);
    }

    if (c == 'q')
      break;
  }

  return 0;
}

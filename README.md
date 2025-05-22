# A simple terminal text editor using only C

This terminal-based text editor i built is a result from
following a tutorial by someone named antirez's kilo text editor
that posted on a website called viewsourcecode.org

Link to the tutorial:
[Link](https://viewsourcecode.org/snaptoken/kilo/)

Antirez github:
[Github](https://github.com/antirez/)
[Kilo TE original code](https://github.com/antirez/kilo)

I own nothing, this repo is purely only for educational purpose
I do not claim the content of this repo to be mine.

### How to run?

I have built a make file so that you can compile the `te.c` with ease, just run:

```bash
make
```

And you will get a binary file compiled called `te`. Run it and it will opens up a simple terminal text editor just using only C

### How to use?

There are 3 key-combination to use within the terminal text edior
- `Ctrl+S` for saving file, you will be prompted what filename it should save the buffer (your text)
- `Ctrl-Q` for quitting, if there are some changes you don't saved yet, it will notify you to do save file, otherwise just spam `Ctrl-Q` 3 times to exit without saving
- `Ctrl-F` for finding a text, it will highlight the matches

If you want to edit an existing file, just run
```bash
te <file>
```

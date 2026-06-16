/* Minimal <termios.h> for the Doom port (raw-tty handling — stubbed). */
#ifndef DOOM_COMPAT_TERMIOS_H
#define DOOM_COMPAT_TERMIOS_H

#define NCCS 32
struct termios {
	unsigned c_iflag, c_oflag, c_cflag, c_lflag;
	unsigned char c_cc[NCCS];
};

#define ECHO      0x0008
#define ICANON    0x0002
#define TCSAFLUSH 2
#define VMIN      6
#define VTIME     5

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#endif

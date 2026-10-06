#ifndef MICH64_TERMIOS_H
#define MICH64_TERMIOS_H

#include <sys/ioctl.h>

// The termios layout and flag set the kernel carries, spelled the way the
// headers a ported program includes spell them. A flag outside this set is
// refused by TCGETS/TCSETS rather than half-applied.
#define NCCS 19
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VSUSP 10

#define ICRNL 0x100
#define OPOST 0x1
#define ONLCR 0x4
#define ISIG 0x1
#define ICANON 0x2
#define ECHO 0x8

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

// The line discipline applies a change when TCSETS arrives and never
// queues one, so all three actions the header spells mean the same thing
// here; the parameter is validated and otherwise carried through.
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int descriptor, struct termios *termios);
int tcsetattr(int descriptor, int actions, const struct termios *termios);

#endif

#ifndef MICH64_SYS_IOCTL_H
#define MICH64_SYS_IOCTL_H

// The tty requests the profile answers. The numbers are the Linux ones, so
// a program that already passes them keeps working; the kernel refuses a
// request aimed at a descriptor that is not a tty with ENOTTY.
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

int ioctl(int descriptor, unsigned long request, void *argument);

#endif

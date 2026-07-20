// sshterm.h - GUI SSH terminal app for MayteraOS
// A windowed VT100 terminal driven by the net/ssh/ssh2 client. Connection
// parameters are read from /CONFIG/SSH.CFG (host=/user=/pass=/port=).
#ifndef SSHTERM_H
#define SSHTERM_H

// Launch the SSH terminal (non-blocking: connects in a background pump thread).
void sshterm_launch(void);

#endif // SSHTERM_H

// MayteraOS port: no-op stub of the GUI command socket. curaslice is CLI-only
// (STL in, g-code file out); there is no libArcus/TCP front-end and the
// freestanding userland has no BSD sockets here. Every method is inert:
// sends are dropped, receives return zero-filled buffers. See
// CURAENGINE_PORT_PLAN.md section 3.5 (socket.cpp -> STUB, CLI mode only).
//
// The original networking implementation is preserved in the vendored upstream
// tree; this file intentionally replaces it wholesale rather than #ifdef-ing
// out each call site.

#include <string.h>

#include "socket.h"

ClientSocket::ClientSocket()
{
    sockfd = -1;
}

void ClientSocket::connectTo(const char* /*host*/, int /*port*/)
{
    // Networking disabled on MayteraOS: stay disconnected.
    sockfd = -1;
}

ClientSocket::~ClientSocket()
{
    close();
}

void ClientSocket::sendNr(int /*nr*/)
{
    // no-op
}

void ClientSocket::sendAll(const void* /*data*/, int /*length*/)
{
    // no-op
}

int ClientSocket::recvNr()
{
    return 0;
}

void ClientSocket::recvAll(void* data, int length)
{
    // No connection: hand back a zero-filled buffer so callers see empty input.
    if (data && length > 0)
        memset(data, 0, (size_t)length);
}

void ClientSocket::close()
{
    sockfd = -1;
}

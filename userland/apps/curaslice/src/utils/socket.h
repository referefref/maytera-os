#ifndef SOCKET_H
#define SOCKET_H

// MayteraOS port: the GUI command socket (libArcus/TCP front-end) is not used.
// curaslice runs CLI-only and emits g-code to a file, so ClientSocket is a
// no-op stub. connectTo takes const char* here (not std::string) so this header
// pulls in no string type and no networking. See CURAENGINE_PORT_PLAN.md
// section 3.5 (socket.cpp -> STUB, CLI mode only).

class ClientSocket
{
    int sockfd;
public:
    ClientSocket();
    ~ClientSocket();

    void connectTo(const char* host, int port);

    void sendNr(int nr);
    void sendAll(const void* data, int length);
    int recvNr();
    void recvAll(void* data, int length);

    void close();
};

#endif//SOCKET_H

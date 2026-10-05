#ifndef MICH64_NETINET_IN_H
#define MICH64_NETINET_IN_H

#include <sys/socket.h>

typedef unsigned short in_port_t;
typedef unsigned int in_addr_t;

#define IPPROTO_TCP 6u
#define IPPROTO_UDP 17u

#define INADDR_ANY 0u
#define INADDR_LOOPBACK 0x7F000001u

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

// The machine is little endian, so the network byte order the wire and
// the kernel addresses carry is a byte swap of the host values.
in_port_t htons(in_port_t value);
in_port_t ntohs(in_port_t value);
in_addr_t htonl(in_addr_t value);
in_addr_t ntohl(in_addr_t value);

#endif

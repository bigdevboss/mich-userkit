#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <mich/syscall.h>

// Exercises the POSIX stream surface against the host echo the smoke runner
// bridges to 10.0.2.4:8080. The virtio-net capsule below owns the NIC, this
// process owns nothing but descriptors, so a full exchange proves the whole
// path end to end: route lookup, active open, the connect park, chunked
// stream send, the receive wake, and the ordered half close.

#define TCPWIRE_PEER 0x0A000204u
#define TCPWIRE_PEER_PORT 8080u
#define TCPWIRE_PAYLOAD 1024u
// The capsule needs a few hundred ticks for DHCP, so the early connects have
// no route yet and answer EHOSTUNREACH; the budget is patient the way the
// dns probe one is, and a dead forward fails instead of hanging.
#define TCPWIRE_CONNECT_ATTEMPTS 40u
#define TCPWIRE_CONNECT_DELAY_TICKS 25u

// Static: two payload buffers do not fit the eight page module stack.
static unsigned char block[TCPWIRE_PAYLOAD];
static unsigned char mirror[TCPWIRE_PAYLOAD];

static void fill_peer(struct sockaddr_in *address) {
    for (unsigned int index = 0; index < sizeof(*address); index++)
        ((unsigned char *)address)[index] = 0;
    address->sin_family = AF_INET;
    address->sin_port = htons(TCPWIRE_PEER_PORT);
    address->sin_addr.s_addr = htonl(TCPWIRE_PEER);
}

int main(void) {
    for (unsigned int index = 0; index < sizeof(block); index++)
        block[index] = (unsigned char)(index * 13u + 7u);

    int descriptor = -1;
    for (unsigned int attempt = 0;
         attempt < TCPWIRE_CONNECT_ATTEMPTS && descriptor < 0; attempt++) {
        int fresh = socket(AF_INET, SOCK_STREAM, 0);
        if (fresh >= 0) {
            struct sockaddr_in peer;
            fill_peer(&peer);
            if (!connect(fresh, (struct sockaddr *)&peer, sizeof(peer))) {
                descriptor = fresh;
                break;
            }
            close(fresh);
        }
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = (long)TCPWIRE_CONNECT_DELAY_TICKS * 10 * 1000 * 1000;
        struct timespec left;
        nanosleep(&delay, &left);
    }
    if (descriptor < 0) {
        mich_write("Mich tcpwire: connect FAIL\n");
        return 1;
    }
    mich_write("Mich tcpwire: stream connected\n");

    // Two staging chunks on the way out, then the echo drains in whatever
    // segments the wire hands back; only the byte count and the pattern
    // carry meaning.
    if (send(descriptor, block, sizeof(block), 0) !=
        (ssize_t)sizeof(block)) {
        mich_write("Mich tcpwire: send FAIL\n");
        return 1;
    }
    unsigned int received = 0;
    while (received < sizeof(block)) {
        ssize_t got = recv(descriptor, mirror + received,
                           sizeof(block) - received, 0);
        if (got <= 0) {
            mich_write("Mich tcpwire: receive FAIL\n");
            return 1;
        }
        received += (unsigned int)got;
    }
    for (unsigned int index = 0; index < sizeof(block); index++)
        if (mirror[index] != block[index]) {
            mich_write("Mich tcpwire: echo mismatch FAIL\n");
            return 1;
        }
    mich_write("Mich tcpwire: echo round trip pass\n");

    // The forward address is the one connect stored, so the peer name is
    // the wire truth rather than a hope.
    struct sockaddr_in named;
    socklen_t named_length = sizeof(named);
    if (getpeername(descriptor, (struct sockaddr *)&named, &named_length) ||
        named_length != sizeof(named) || named.sin_family != AF_INET ||
        ntohs(named.sin_port) != TCPWIRE_PEER_PORT ||
        ntohl(named.sin_addr.s_addr) != TCPWIRE_PEER) {
        mich_write("Mich tcpwire: peer name FAIL\n");
        return 1;
    }
    mich_write("Mich tcpwire: peer name pass\n");

    // The write half closes, the echo answers with its own end, and the
    // drained read half answers zero.
    if (shutdown(descriptor, SHUT_WR)) {
        mich_write("Mich tcpwire: shutdown FAIL\n");
        return 1;
    }
    ssize_t ended = recv(descriptor, mirror, 1, 0);
    if (ended != 0) {
        mich_write("Mich tcpwire: half close FAIL\n");
        return 1;
    }
    if (close(descriptor)) {
        mich_write("Mich tcpwire: close FAIL\n");
        return 1;
    }
    mich_write("Mich tcpwire: POSIX TCP wire pass\n");
    return 0;
}

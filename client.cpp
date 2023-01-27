#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <netdb.h>
#include <thread>
#include <atomic>
#include <iostream>

using namespace std;

// CLIENT INSTANCE
// DEFINE YOUR OWN PARAMETERS BELOW
// MESSAGE PATTERN [TAG] Message
// SYSTEM TAGS:
//      [LOGIN]
//      [LOGOUT]
//      [TOPICS]
//      [SUB]
//      [UNSUB]
//      [HEARTBEAT]
//      every message sent with a different tag will add a message to the topic or create a new one.

const string HEARTBEAT = "[HEARTBEAT]";
#define HEARTBEAT_MSG_TIME 2000
clock_t start = clock();

int sock;
atomic<bool> quit{false};

void ctrl_c(int) {
    quit = true;
    shutdown(sock, SHUT_RDWR);
    exit(1);
}

void connectToHost(const char * host, const char * port) {
    addrinfo hints {.ai_protocol = IPPROTO_TCP};
    addrinfo *resolved;
    if(int err = getaddrinfo(host, port, &hints, &resolved)) {
        fprintf(stderr, "Resolving address failed: %s\n", gai_strerror(err));
        exit(1);
    }
    sock = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if(connect(sock, resolved->ai_addr, resolved->ai_addrlen)) {
        fprintf(stderr, "Failed to connect: %s\n", strerror(errno));
        exit(1);
    }
    freeaddrinfo(resolved);
}

void doWork() {
    while (!quit) {
        fflush(stdout);
        fflush(stdin);
        char buf[255] = "";
        char message[256];
        //fgets adds \n
        ::fgets(message, 256, stdin);
        ::strcat(buf, message);
        if ((int) strlen(buf) != write(sock, buf, strlen(buf))) {
            return;
        } else {
            fflush(stdout);
            fflush(stdin);
            start = clock();
            continue;
        }
    }
}

void doRead() {

    while (!quit) {
        cin.clear();
        char buf[255], *eol;
        int pos{0};
        while(true) {
            // dane z sieci zapisz do bufora, zaczynając od miejsca za wcześniej zapisanymi danymi
            int bytesRead = read(sock, buf + pos, 255 - pos);
            if (bytesRead < 1) return;
            else {
                // zaktualizuj ile łącznie danych jest w buforze
                pos += bytesRead;
                // zapisz znak '\0' na końcu danych
                buf[pos] = 0;

                // dopóki w danych jest znak nowej linii
                while (nullptr != (eol = strchr(buf, '\n'))) {

                    // wykonaj komendę
                    cout << buf << endl;
                    start = clock();
                    // usuń komendę z bufora

                    // (pomocnicze) wylicz długość komendy
                    int cmdLen = (eol - buf) + 1;
                    // przesuń pozostałe dane i znak '\0' na początek bufora
                    memmove(buf, eol + 1, pos - cmdLen + 1);
                    // zaktualizuj zmienną pamiętającą ile danych jest w buforze
                    pos -= cmdLen;

                    fflush(stdout);
                    fflush(stdin);
                }

                // jeżeli w 255 znakach nie ma '\n', wyjdź.
                if (pos == 255) {
                    break;
                }
            }
        }
    }
}

void doHeartbeat() {
    while (!quit) {
        if ((float) (::clock() - start) / 1000 > HEARTBEAT_MSG_TIME) {
            char buf[255] = "";
            ::strcat(buf, HEARTBEAT.c_str());
            ::strcat(buf, "\n");
            if ((int) strlen(buf) != write(sock, buf, strlen(buf))) {
                return;
            } else {
                start = clock();
            }
        }
    }
}

int main(int argc, char ** argv) {
    if(argc !=3) {
        fprintf(stderr, "Usage:\n%s <ip> <port>\n", argv[0]);
        return 1;
    }

    connectToHost(argv[1], argv[2]);

    signal(SIGINT, ctrl_c);
    fflush(stdout);
    fflush(stdin);

    thread threads[9];
    for(auto &t : threads)
        t = thread(doWork);
    thread threads_read[9];
    for(auto &t : threads_read)
        t = thread(doRead);
    thread heartbeat[1];
    for(auto &t : heartbeat)
        t = thread(doRead);
    doHeartbeat();
    doWork();
    doRead();
    for(auto &t : threads)
        t.join();
    for(auto &t : threads_read)
        t.join();
    for(auto &t : heartbeat)
        t.join();



    const linger lv {.l_onoff=1, .l_linger=60};
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lv, sizeof(lv));

    shutdown(sock, SHUT_RDWR);
    close(sock);
    return 0;
}

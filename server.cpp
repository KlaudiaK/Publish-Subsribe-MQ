#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "error.h"
#include <poll.h>
#include <signal.h>

// server socket
int servFd;

// data for poll
int descrCapacity = 16;
int descrCount = 1;
pollfd * descr;

// handles SIGINT
void ctrl_c(int);

// sends data to clientFds excluding fd
void sendToAllBut(int fd, char * buffer, int count);

// converts cstring to port
uint16_t readPort(char * txt);

// sets SO_REUSEADDR
void setReuseAddr(int sock);

void eventOnServFd(int revents) {
    // Wszystko co nie jest POLLIN na gnieździe nasłuchującym jest traktowane
    // jako błąd wyłączający aplikację
    if(revents & ~POLLIN){
        error(0, errno, "Event %x on server socket", revents);
        ctrl_c(SIGINT);
    }

    if(revents & POLLIN){
        sockaddr_in clientAddr{};
        socklen_t clientAddrSize = sizeof(clientAddr);

        auto clientFd = accept(servFd, (sockaddr*) &clientAddr, &clientAddrSize);
        if(clientFd == -1) error(1, errno, "accept failed");

        if(descrCount == descrCapacity){
            // Skończyło się miejsce w descr - podwój pojemność
            descrCapacity<<=1;
            descr = (pollfd*) realloc(descr, sizeof(pollfd)*descrCapacity);
        }

        descr[descrCount].fd = clientFd;
        descr[descrCount].events = POLLIN|POLLHUP;
        descrCount++;

        printf("new connection from: %s:%hu (fd: %d)\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), clientFd);
    }
}

void eventOnClientFd(int indexInDescr) {
    auto clientFd = descr[indexInDescr].fd;
    auto revents = descr[indexInDescr].revents;

    if(revents & POLLIN){
        char buf[255], *eol;
        int pos{0};
        // dane z sieci zapisz do bufora, zaczynając od miejsca za wcześniej zapisanymi danymi
        int bytesRead = read(clientFd, buf + pos, 255 - pos);
        if (bytesRead < 1)
            revents |= POLLERR;
        else {
            // zaktualizuj ile łącznie danych jest w buforze
            pos+=bytesRead;
            // zapisz znak '\0' na końcu danych
            buf[pos] = 0;

            // dopóki w danych jest znak nowej linii
            while(nullptr != (eol = strchr(buf, '\n'))){

                // wykonaj komendę
                sendToAllBut(clientFd, buf, bytesRead);
                printf("sending to desc: %d\nmessage: %s\n", clientFd, buf);
                // usuń komendę z bufora

                // (pomocnicze) wylicz długość komendy
                int cmdLen = (eol-buf)+1;
                // przesuń pozostałe dane i znak '\0' na początek bufora
                memmove(buf, eol+1, pos-cmdLen+1);
                // zaktualizuj zmienną pamiętającą ile danych jest w buforze
                pos -= cmdLen;

                fflush(stdout);
            }

            // jeżeli w 255 znakach nie ma '\n', wyjdź.
            if(pos == 255)
                return;

        }
    }

    if(revents & ~POLLIN){
        printf("removing %d\n", clientFd);

        // remove from description of watched files for poll
        descr[indexInDescr] = descr[descrCount-1];
        descrCount--;

        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
    }
}

int main(int argc, char ** argv){
    // get and validate port number
    if(argc != 2) error(1, 0, "Need 1 arg (port)");
    auto port = readPort(argv[1]);

    // create socket
    servFd = socket(AF_INET, SOCK_STREAM, 0);
    if(servFd == -1) error(1, errno, "socket failed");

    // graceful ctrl+c exit
    signal(SIGINT, ctrl_c);
    // prevent dead sockets from throwing pipe errors on write
    signal(SIGPIPE, SIG_IGN);

    setReuseAddr(servFd);

    // bind to any address and port provided in arguments
    sockaddr_in serverAddr{.sin_family=AF_INET, .sin_port=htons((short)port), .sin_addr={INADDR_ANY}};
    int res = bind(servFd, (sockaddr*) &serverAddr, sizeof(serverAddr));
    if(res) error(1, errno, "bind failed");

    // enter listening mode
    res = listen(servFd, 1);
    if(res) error(1, errno, "listen failed");

    descr = (pollfd*) malloc(sizeof(pollfd)*descrCapacity);

    descr[0].fd = servFd;
    descr[0].events = POLLIN;

    while(true){
        int ready = poll(descr, descrCount, -1);
        if(ready == -1){
            error(0, errno, "poll failed");
            ctrl_c(SIGINT);
        }

        for(int i = 0 ; i < descrCount && ready > 0 ; ++i){
            if(descr[i].revents){
                if(descr[i].fd == servFd)
                    eventOnServFd(descr[i].revents);
                else {
                    eventOnClientFd(i);
                }
                ready--;
            }
        }
    }
}

uint16_t readPort(char * txt){
    char * ptr;
    auto port = strtol(txt, &ptr, 10);
    if(*ptr!=0 || port<1 || (port>((1<<16)-1))) error(1,0,"illegal argument %s", txt);
    return port;
}

void setReuseAddr(int sock){
    const int one = 1;
    int res = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(res) error(1,errno, "setsockopt failed");
}

void ctrl_c(int){
    for(int i = 1 ; i < descrCount; ++i){
        shutdown(descr[i].fd, SHUT_RDWR);
        close(descr[i].fd);
    }
    close(servFd);
    printf("Closing server\n");
    exit(0);
}

void sendToAllBut(int fd, char * buffer, int count){
    int i = 1;
    while(i < descrCount){
        int clientFd = descr[i].fd;
        if(clientFd == fd) {
            i++;
            continue;
        }

        int res = write(clientFd, buffer, count);
        if(res!=count){
            printf("removing %d\n", clientFd);
            shutdown(clientFd, SHUT_RDWR);
            close(clientFd);
            descr[i] = descr[descrCount-1];
            descrCount--;
            continue;

        }
        i++;
    }
}

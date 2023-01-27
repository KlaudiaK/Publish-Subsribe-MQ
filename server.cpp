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
#include "map"
#include "list"
#include "vector"
#include "string"
#include <cstring>
#include <algorithm>
#include <iostream>

using namespace std;

// SERVER INSTANCE
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

#define LOGIN_ATTEMPT_NUMBER 3
#define MAX_NUMBER_OF_USERS 5
#define MAX_NUMBER_OF_TOPICS 10
const string KEY = "7312";
const string LOGIN_TAG =  "[LOGIN]";
const string LOGOUT_TAG = "[LOGOUT]";
const string TOPICS_TAG = "[TOPICS]";
const string SUBSCRIBE_TAG = "[SUB]";
const string UNSUBSCRIBE_TAG = "[UNSUB]";
const string HEARTBEAT = "[HEARTBEAT]";

vector<int> usersDescriptors = vector<int>();
int current_number_of_users = 0;
map<int, bool> logged_in = map<int, bool>();
map<int, list<string>> subscribed = map<int, list<string>>();
map<int, int> login_attempts = map<int, int>();
vector<string> topics = vector<string>();

bool checkIfUserLoggedInByDescriptor(int fd);
void tryLogInUserWithDescriptor(int fd, string key);
bool addUser(int fd);
void removeUser(int fd);
void unsubscribe(int fd, string topic);
void subscribe(int fd, string topic);
void addTopic(int fd, string topic);
void removeTopic(string topic);
pair<string, string> splitMessage(string message);
void sendToFd(int fd, char * buffer, int count);
bool checkSystemTag(string tag);
void displayTopics(int fd);
void strip(std::string& str);

// server socket
int servFd;

// data for poll
int descrCapacity = MAX_NUMBER_OF_USERS;
int descrCount = 1;
pollfd * descr;

// handles SIGINT
void ctrl_c(int);

// sends data to clientFds excluding fd
void sendToAllBut(int fd, char * buffer, int count);
void sendToAllButWithinTopic(int fd, char * buffer, int count, string topic);

// converts cstring to port
uint16_t readPort(char * txt);

// sets SO_REUSEADDR
void setReuseAddr(int sock);

void eventOnServFd(int revents) {
    fflush(stdout);
    fflush(stdin);
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

        if (addUser(clientFd)) {
            descr[descrCount].fd = clientFd;
            descr[descrCount].events = POLLIN | POLLHUP;
            descrCount++;

            printf("new connection from: %s:%hu (fd: %d)\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port),
                   clientFd);
        }
    }
    fflush(stdout);
    fflush(stdin);
}

void eventOnClientFd(int indexInDescr) {
    auto clientFd = descr[indexInDescr].fd;
    auto revents = descr[indexInDescr].revents;

    if(revents & POLLIN){
        fflush(stdout);
        fflush(stdin);
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

                // wykonaj komendę - parsowanie
                string tag = splitMessage(buf).first;
                string message = splitMessage(buf).second;
                strip(tag);
                strip(message);
                //sprawdzanie tagu
                if (tag.empty()) {
                    sendToFd(clientFd, "Please provide a tag. (Message pattern: [TAG] message)\n", 100);
                } else {
                    //sprawdzanie zalogowania i tagu login
                    if (checkIfUserLoggedInByDescriptor(clientFd) || checkSystemTag(tag)) {
                        if (tag.compare(TOPICS_TAG) == 0) {
                            displayTopics(clientFd);
                        } else if (tag.compare(LOGIN_TAG) == 0) {
                            tryLogInUserWithDescriptor(clientFd, message);
                        } else if (tag.compare(LOGOUT_TAG) == 0) {
                            removeUser(clientFd);
                            shutdown(clientFd, SHUT_RDWR);
                            close(clientFd);
                        } else if (tag.compare(UNSUBSCRIBE_TAG) == 0) {
                            unsubscribe(clientFd, message);
                        } else if (tag.compare(SUBSCRIBE_TAG) == 0) {
                            subscribe(clientFd, message);
                        } else if (tag.compare(HEARTBEAT) == 0) {
                            //do nothing
                        } else {
                            subscribe(clientFd, tag);
                            addTopic(clientFd, tag);
                            sendToAllButWithinTopic(clientFd, buf, bytesRead, tag);
                            cout << "sending from desc: " << clientFd << "\nmessage: " << buf << endl;
                        }
                    } else {
                        sendToFd(clientFd, "Please login first. (Message pattern: [LOGIN] /key/)\n", 100);
                    }
                }

                // usuń komendę z bufora
                // (pomocnicze) wylicz długość komendy
                int cmdLen = (eol-buf)+1;
                // przesuń pozostałe dane i znak '\0' na początek bufora
                memmove(buf, eol+1, pos-cmdLen+1);
                // zaktualizuj zmienną pamiętającą ile danych jest w buforze
                pos -= cmdLen;

                fflush(stdout);
                fflush(stdin);
            }

            // jeżeli w 255 znakach nie ma '\n', wyjdź.
            if(pos == 255)
                return;

        }
    }

    if(revents & ~POLLIN){
        printf("removing %d\n", clientFd);
        removeUser(clientFd);
        // remove from description of watched files for poll
        if (descrCount > 0) {
            descr[indexInDescr] = descr[descrCount-1];
            descrCount--;
        }

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
    int res = ::bind(servFd, (sockaddr*) &serverAddr, sizeof(serverAddr));
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
            removeUser(clientFd);
            shutdown(clientFd, SHUT_RDWR);
            close(clientFd);
            descr[i] = descr[descrCount-1];
            descrCount--;
            continue;

        }
        i++;
    }
}

void sendToAllButWithinTopic(int fd, char * buffer, int count, string topic){
    int i = 1;
    while(i < descrCount) {
        int clientFd = descr[i].fd;
        if(clientFd == fd) {
            i++;
            continue;
        }

        if (std::find(subscribed[clientFd].begin(), subscribed[clientFd].end(), topic) != subscribed[clientFd].end()) {
            cout << "Sending message to " << clientFd << endl;
            int res = write(clientFd, buffer, count);
            if (res != count) {
                printf("removing %d\n", clientFd);
                removeUser(clientFd);
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
                descr[i] = descr[descrCount - 1];
                descrCount--;
                continue;

            }
        }
        i++;
    }
}

void sendToFd(int fd, char * buffer, int count){
    int res = write(fd, buffer, count);
    if(res!=count){
        printf("removing %d\n", fd);
        removeUser(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

bool addUser(int fd) {
    if (current_number_of_users < MAX_NUMBER_OF_USERS) {
        current_number_of_users++;
        usersDescriptors.push_back(fd);
        logged_in.insert(pair<int, bool>(fd, false));
        list<string> list;
        list.emplace_back("log");
        pair<int, std::list<string>> p;
        p.first = fd;
        p.second = list;
        subscribed.insert(p);
        logged_in.insert(pair<int, int>(fd, 0));
        return true;
    } else {
        sendToFd(fd, "Couldn't add user - number of users limit exceeded\n", 100);
        return false;
    }
}

void removeUser(int fd) {
    current_number_of_users--;
    for (auto it = usersDescriptors.begin(); it != usersDescriptors.end(); it++) {
        if (*it == fd) {
            usersDescriptors.erase(it);
            break;
        }
    }
    login_attempts.erase(fd);
    logged_in.erase(fd);
    vector<string> topicsToBeRemoved;
    for (auto topic = subscribed[fd].begin(); topic != subscribed[fd].end(); topic++) {
        bool lastSubscriber = true;
        for (auto user = subscribed.begin(); user != subscribed.end(); user++) {
            if (user->first != fd) {
                for (auto topicInUsers = user->second.begin(); topicInUsers != user->second.end(); topicInUsers++) {
                    string topicStr = *topic;
                    if (topicStr.compare(*topic) == 0) {
                        lastSubscriber = false;
                    }
                }
            }
        }
        if (lastSubscriber) topicsToBeRemoved.emplace_back(*topic);
    }
    for (int i = 0; i < topicsToBeRemoved.size(); ++i) {
        removeTopic(topicsToBeRemoved[i]);
    }
    subscribed.erase(fd);
}

bool checkIfUserLoggedInByDescriptor(int fd) {
    return logged_in[fd];
}

void unsubscribe(int fd, string topic) {
    for (auto it = subscribed[fd].begin(); it != subscribed[fd].end(); it++) {
        if (*it == topic) {
            subscribed[fd].erase(it);
            return;
        }
    }
}

void subscribe(int fd, string topic) {
    if (std::find(subscribed[fd].begin(), subscribed[fd].end(), topic) == subscribed[fd].end()) {
        subscribed[fd].emplace_back(topic);
    }
}

void addTopic(int fd, string topic) {
    if (std::find(topics.begin(), topics.end(), topic) == topics.end()) {
        if (topics.size() < MAX_NUMBER_OF_TOPICS) {
            topics.emplace_back(topic);
        } else {
            sendToFd(fd, "Exceeded maximum number of topics\n", 100);
        }
    }
}

void removeTopic(string topic) {
    for (auto it = topics.begin(); it != topics.end(); it++) {
        if (*it == topic) {
            topics.erase(it);
        }
    }
}

pair<string, string> splitMessage(string message) {
    int index = message.find_first_of("]");
    if(index == std::string::npos) {
        return pair<string, string>("", "");
    }
    pair<string, string> split1 = pair<string, string>(message.substr(0, index + 1), message.substr(index + 1));
    string start = split1.first;
    int index2 = start.find_last_of("[");
    if(index2 == std::string::npos) {
        return pair<string, string>("", "");
    }
    return pair<string, string>(start.substr(index2), message.substr(index + 1));
}

bool checkSystemTag(string tag){
    return (tag.compare(LOGIN_TAG) == 0 ||
            tag.compare(LOGOUT_TAG) == 0 ||
            tag.compare(TOPICS_TAG) == 0 ||
            tag.compare(SUBSCRIBE_TAG) == 0 ||
            tag.compare(UNSUBSCRIBE_TAG) == 0 ||
            tag.compare(HEARTBEAT) == 0
    );
}

void displayTopics(int fd) {
    char buf[255] = "";
    for (int i = 0; i < topics.size(); ++i) {
        ::strcat(buf, topics[i].c_str());
        if (i < topics.size() - 1) {
            ::strcat(buf, " | ");
        }
    }
    ::strcat(buf, "\n");
    sendToFd(fd, buf, sizeof (buf));
}

void tryLogInUserWithDescriptor(int fd, string key) {
    if (login_attempts[fd] < LOGIN_ATTEMPT_NUMBER) {
        strip(key);
        if (key != KEY) {
            sendToFd(fd, "Invalid key. Try again. (Message pattern: [LOGIN] /key/)\n", 100);
            login_attempts[fd]++;
        } else {
            logged_in[fd] = true;
            login_attempts[fd] = 0;
            sendToFd(fd, "Logged in.\n", 100);
        }
    } else {
        sendToFd(fd, "Invalid key. Number of attempts exceeded. Contact your administrator for more information\n", 100);
        removeUser(fd);
    }
}

//https://stackoverflow.com/questions/9358718/similar-function-in-c-to-pythons-strip
void strip(std::string& str)
{
    if (str.length() == 0) {
        return;
    }

    auto start_it = str.begin();
    auto end_it = str.rbegin();
    while (std::isspace(*start_it)) {
        ++start_it;
        if (start_it == str.end()) break;
    }
    while (std::isspace(*end_it)) {
        ++end_it;
        if (end_it == str.rend()) break;
    }
    int start_pos = start_it - str.begin();
    int end_pos = end_it.base() - str.begin();
    str = start_pos <= end_pos ? std::string(start_it, end_it.base()) : "";
}

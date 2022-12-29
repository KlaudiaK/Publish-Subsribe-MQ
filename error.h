#pragma once
#ifdef __GLIBC__
#include <error.h>
#else

// Funkcja error jest rozszerzeniem dostępnym tylko w glibc
// https://www.kernel.org/doc/man-pages/online/pages/man3/error.3.html
// Poniżej przybliżona implementacja funckji error

#include <cstdlib>
#include <cstdio>
#include <cstring>

template <typename ... Args>
void error(int exitStatus, int errorNumber, const char * format, Args ... args){
    char format2 [strlen(format)+5];
    strcpy(format2, format);
    fflush(stdout);
    if(errorNumber) {
        strcat(format2, ": %s\n");
        fprintf(stderr, format2, args..., strerror(errorNumber));
    } else {
        strcat(format2, "\n");
        fprintf(stderr, format2, args...);
    }
    if(exitStatus) exit(exitStatus);
}

#endif

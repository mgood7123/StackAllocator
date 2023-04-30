#include <log.h>

void flush() {
    fflush(stdout);
}
void SA::Logib() {
    printf("\033[32m");
    flush();
}
void SA::Logwb() {
    printf("\033[93m");
    flush();
}
void SA::Logeb() {
    printf("\033[31m");
    flush();
}
void SA::Logr() {
    printf("\033[37m");
    flush();
}
void SA::Loga() {
    Logr();
    std::terminate();
}

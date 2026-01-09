#include <stdlib.h>
#include <stdio.h> //snprintf
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

FILE * debugfile;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char *format, ...) {
    if (!debugfile) return;

    // 1. Bloqueia o acesso ao ficheiro
    pthread_mutex_lock(&log_mutex);
    /*
    // 2. Imprime timestamp (opcional, mas ajuda muito em multithread)
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(debugfile, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
*/
    // 3. Imprime a mensagem
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);
    
    // Forçar a escrita no disco imediatamente (bom para crash debug)
    fflush(debugfile);
    // 4. Liberta o acesso
    pthread_mutex_unlock(&log_mutex);
}

/*void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}*/

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}
#ifndef GAME_SESSION_H
#define GAME_SESSION_H

#include <pthread.h>

typedef struct {
    int active;           // Flag para parar as threads
    int fd_req;           // Ler do cliente
    int fd_notif;         // Escrever para o cliente
    
    // Dados do Jogo
    pthread_mutex_t lock; // Protege o acesso ao jogo
    char *grid;           // O array do tabuleiro
    int width;
    int height;
    int pacman_x;
    int pacman_y;
    int tempo;
    int score;
    int game_over;
    int victory;
} GameSession;

#endif
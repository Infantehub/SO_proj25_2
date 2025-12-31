#ifndef SERVER_H
#define SERVER_H

#include "board.h"

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2

//Game session structure defined in board.h for logical header reasons

typedef struct {
    board_t *board;
    GameSession *game_session;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct{
    board_t *board;
    GameSession *game_session;
} pacman_thread_arg_t;

#endif
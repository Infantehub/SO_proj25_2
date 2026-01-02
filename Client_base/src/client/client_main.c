#include "../../include/display.h"
#include "../../include/protocol.h"
#include "../../include/debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

Board board;
bool stop_execution = false;
int tempo;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;

    while (!stop_execution) {
        // 1. Recebe FORA do lock (não bloqueia as outras threads)
        Board new_data = receive_board_update();
        if (!new_data.data) {
            if (new_data.data) free(new_data.data);
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        // 2. Entra, troca os dados e sai do lock
        pthread_mutex_lock(&mutex);
        
        // 3. Liberta a memória do frame anterior antes de guardar o novo
        if (board.data != NULL) {
            free(board.data);
        }
        
        // 4. Atualiza o board global
        board = new_data; 
        tempo = board.tempo;

        // 5. Verifica condições de paragem
        if (board.game_over == 1 || board.victory == 1) {
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }
        
        pthread_mutex_unlock(&mutex);
    }

    // 6. Limpeza final ao sair pertence à ultima thread a acabar aka ncurses_thread
    
    return NULL;
}

void screen_refresh(Board game_board) {
    draw_board_client(game_board);
    refresh_screen();     
}

void* ncurses_thread(void *arg) {
    (void)arg;
    sleep_ms(tempo / 2);
    while (true) {
        sleep_ms(tempo);
        pthread_mutex_lock(&mutex);
        if(stop_execution) {
            screen_refresh(board);
            pthread_mutex_unlock(&mutex);
            break;
        }
        screen_refresh(board);
        pthread_mutex_unlock(&mutex);
    }

    pthread_mutex_lock(&mutex);
    if (board.data) {
        free(board.data);
        board.data = NULL;
    }
    pthread_mutex_unlock(&mutex);

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    char register_pipe[MAX_PIPE_PATH_LENGTH];
    memset(register_pipe, 0, MAX_PIPE_PATH_LENGTH);
    strncpy(register_pipe, argv[2], MAX_PIPE_PATH_LENGTH);
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    //TODO: abrir ficheiro de comandos se for fornecido
    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    memset(req_pipe_path, 0, MAX_PIPE_PATH_LENGTH);
    memset(notif_pipe_path, 0, MAX_PIPE_PATH_LENGTH);

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id, ncurses_thread_id;
    pthread_create(&ncurses_thread_id, NULL, ncurses_thread, (void*)&board);
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    if (receiver_thread_id == 0) {
        debug("Failed to create receiver thread\n");
        pacman_disconnect();
        return 1;
    }
    if (ncurses_thread_id == 0) {
        debug("Failed to create ncurses thread\n");
        pacman_disconnect();
        return 1;
    }

    terminal_init();
    set_timeout(500);

    char command;
    int ch;

    while (1) {

        pthread_mutex_lock(&mutex);
        if (stop_execution){
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            // Interactive input
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        debug("Command: %c\n", command);

        pacman_play(command);

    }

    pthread_join(receiver_thread_id, NULL);
    pthread_join(ncurses_thread_id, NULL);

    debug("Client exiting...\n");
    if (pacman_disconnect() != 0) {
        debug("Failed to disconnect from server\n");
    }
    debug("Client disconnected from server\n");

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);

    terminal_cleanup();

    return 0;
}

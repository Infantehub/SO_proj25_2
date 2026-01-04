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
#include <signal.h>

Board board;
bool stop_execution = false;
int tempo = -1;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*Função simplificada para atualizar a tela*/
void screen_refresh(Board game_board) {
    draw_board_client(game_board);
    refresh_screen();     
}

static void *receiver_thread(void *arg) {
    (void)arg;

    while (!stop_execution) {

        // 1. Recebe novo tabuleiro 
        // FORA do lock para não bloquear as outras threads
        Board new_data = receive_board_update();

        if (new_data.data == NULL) {
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
        
        // 4. Atualiza o board global e o tempo
        board = new_data; 
        tempo = board.tempo;

        // 5. Verifica condições de paragem
        if (board.game_over == 1 || board.victory == 1) {
            stop_execution = true;
        }
        
        pthread_mutex_unlock(&mutex);
    }

    // 6. Limpeza final ao sair pertence à ultima thread a acabar aka ncurses_thread
    
    return NULL;
}

void* ncurses_thread(void *arg) {
    (void)arg;

    // 2. Loop de refresh
    while (true) {
        pthread_mutex_lock(&mutex);
        if (tempo < 0){
            pthread_mutex_unlock(&mutex);
            sleep_ms(100);
            continue;
        }

        // 3. Esperar pelo tempo definido
        int local_tempo = board.tempo;
        pthread_mutex_unlock(&mutex);

        sleep_ms(local_tempo);

        // 4. Verificar se deve parar
        if(stop_execution) {
            // 4.1 Último refresh antes de sair
            if (board.data != NULL) {
                screen_refresh(board);
            }
            pthread_mutex_unlock(&mutex);
            break;
        }

        // 5. Fazer refresh do ecrã (normal)
        if (board.data){
            screen_refresh(board);
        }
        pthread_mutex_unlock(&mutex);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    // 1. Processar os argumentos
    const char *client_id = argv[1];
    char register_pipe[MAX_PIPE_PATH_LENGTH];
    memset(register_pipe, 0, MAX_PIPE_PATH_LENGTH);
    strncpy(register_pipe, argv[2], MAX_PIPE_PATH_LENGTH);
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    //TODO: 2. Abrir ficheiro de comandos se for fornecido
    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    // 3. Preparar os pipes do cliente
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    memset(req_pipe_path, 0, MAX_PIPE_PATH_LENGTH);
    memset(notif_pipe_path, 0, MAX_PIPE_PATH_LENGTH);

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    // 4. Conectar ao servidor
    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    // 5. Inicializar display
    terminal_init();
    set_timeout(500);

    // 6. Criar threads de receção e ncurses
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

    char command;
    int ch;

    // 7. Loop principal de input
    while (1) {

        pthread_mutex_lock(&mutex);
        // 8. Verificar se deve parar
        if (stop_execution){
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        // 9. Obter comando do ficheiro
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
            int wait_for = (tempo > 0) ? tempo * 10 : 200;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } 
        else {
            // 10. input interativo
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
        }

        debug("Command: %c\n", command);

        pacman_play(command);

    }

    debug("Client exiting...\n");
    if (pacman_disconnect() != 0) {
        debug("Failed to disconnect from server\n");
    }
    debug("Client disconnected from server\n");

    pthread_join(receiver_thread_id, NULL);
    pthread_join(ncurses_thread_id, NULL);

    // 6. Limpeza da memória do board --> ativado por stop_execution = true
    pthread_mutex_lock(&mutex);
    if (board.data != NULL) {
        free(board.data);
        board.data = NULL;
    }
    pthread_mutex_unlock(&mutex);

    if (cmd_fp)
        fclose(cmd_fp);

    pthread_mutex_destroy(&mutex);

    terminal_cleanup();

    close_debug_file();
    return 0;
}

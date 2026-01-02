#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>

#include "protocol.h"
#include "debug.h"
#include "server.h"

static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Só para inicializar o jogo (fictício)
void init_session(GameSession *s) {
    s->width = 5;
    s->height = 5;
    s->grid = malloc(s->width * s->height);
    s->tempo = 10;
    s->score = 0;
    s->game_over = 0;
    s->victory = 0;

    // Encher com pontos
    memset(s->grid, '.', s->width * s->height);

    // Paredes nas bordas (exemplo simples)
    for(int i=0; i<s->width; i++) {
        s->grid[i] = 'W'; // Topo
        s->grid[(s->height-1)*s->width + i] = 'W'; // Fundo
    }
    for(int i=0; i<s->height; i++) {
        s->grid[i*s->width] = 'W'; // Esquerda
        s->grid[i*s->width + (s->width-1)] = 'W'; // Direita
    }

    // Colocar Pacman no meio
    s->pacman_x = s->width / 2;
    s->pacman_y = s->height / 2;
    s->grid[s->pacman_y * s->width + s->pacman_x] = 'C'; // 'C' é o Pacman
}

/*Esta tarefa é responsável por receber o input do cliente
e por fazer a movimentação do pacman, tal como retornar o estado do pacman*/
void* input_handler_thread(void* arg){
    pacman_thread_arg_t *pac_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pac_arg->board;
    GameSession *session = pac_arg->game_session;

    free(pac_arg);
    
    char buf[2 * sizeof(char)];
    int *retval = malloc(sizeof(int));
    *retval = 0; // Inicializar por segurança

    //Loop principal da thread de input
    while(1) {

        // 2. Ler (bloqueante, sem lock, o que é bom)
        memset(buf, 0, sizeof(buf));
        int n = read(session->fd_req, &buf, sizeof(buf));
        debug("Read %d bytes from client request pipe\n", n);

        if (n <= 0) { //Erro
            debug("Error reading client request\n");
            pthread_mutex_lock(&session->lock);
            session->active = 0;
            pthread_mutex_unlock(&session->lock);
            break;
        }

        // 3. Trancar Sessão para processar
        pthread_mutex_lock(&session->lock);
        
        char opcode = buf[0];

        // 4. Processar comando

        // 4.1 Se o cliente pediu para disconectar (sempre para sair)
        if (opcode == OP_CODE_DISCONNECT) {
            session->active = 0;
            close(session->fd_req);
            close(session->fd_notif);
            pthread_mutex_unlock(&session->lock); // Destrancar antes de sair
            *retval = QUIT_GAME;
            return (void*) retval;
        }
        
        // 4.2 Se o cliente enviou um comando de jogo
        if (opcode == OP_CODE_PLAY) {
            char command = buf[1];
            
            // QUIT
            if (command == 'Q') {
                *retval = QUIT_GAME;
                session->active = 0;
                close(session->fd_req);
                close(session->fd_notif);
                pthread_mutex_unlock(&session->lock); // Destrancar antes de sair
                return (void*) retval;
            }

            // 5. Mover Pacman
            pthread_rwlock_wrlock(&board->state_lock); // Trancar o tabuleiro para mexer
            
            command_t play = { .command = command, .turns = 1 };
            debug("KEY %c\n", play.command);

            int result = move_pacman(board, 0, &play);
            
            pthread_rwlock_unlock(&board->state_lock); // Já acabámos de mexer no tabuleiro, podemos destrancar

            if (result == REACHED_PORTAL) {
                *retval = NEXT_LEVEL;
                pthread_mutex_unlock(&session->lock); // Destranca sessão
                break; // Sai do loop
            }

            if (result == DEAD_PACMAN) {
                *retval = QUIT_GAME;
                session->active = 0;
                pthread_mutex_unlock(&session->lock); // Destranca sessão
                break; // Sai do loop
            }
        }

        // 6. Fim da iteração: destranca a sessão para a próxima volta
        pthread_mutex_unlock(&session->lock);
    }
    
    return (void*) retval;
}

void send_board_to_client(GameSession *session) {
    int header_size = 1*sizeof(char) + 6 * sizeof(int); // OP + 6 inteiros
    int board_size = session->width * session->height;

    debug("Preparing to send board update to client\n");
    char *buffer = malloc(header_size);
    if (!buffer) return;

    int p = 0;
    // 1. OP CODE
    buffer[p++] = OP_CODE_BOARD; // Deve ser 4 no protocol.h

    // 2. Inteiros (memcpy para segurança binária)
    memcpy(buffer + p, &session->width, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->height, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->tempo, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->victory, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->game_over, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->score, sizeof(int)); p += sizeof(int);

    // 3. Enviar cabeçalho primeiro
    debug("Sending board update to client:\n");
    if (write(session->fd_notif, buffer, header_size) == -1 ) {
        session->active = 0;
        return;
    }
    
    // 4. Preparar e enviar o tabuleiro
    char *board_buffer = malloc(board_size);
    if (!board_buffer) {
        free(buffer);
        return;
    }
    memcpy(board_buffer, session->grid, board_size);

    debug("Sending board grid to client:\n");
    write(session->fd_notif, board_buffer, board_size);

    // 5. Limpar
    free(buffer);
    free(board_buffer);
}

void translate_board_to_session(board_t *board, GameSession *session) {
    debug("Translating board to session format\n");
    // Assegurar que o grid da sessão está alocado
    if (!session->grid) {
        session->grid = malloc(board->width * board->height);
    }
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = get_board_index(board, x, y);
            if(board->board[idx].content == 'W') {
                session->grid[idx] = '#'; // Wall
            }
            else if(board->board[idx].content == 'P') {
                session->grid[idx] = 'C'; // Pacman
            }
            else if(board->board[idx].content == 'M') {
                session->grid[idx] = 'M'; // Monster
            }
            else if(board->board[idx].has_portal) {
                session->grid[idx] = '@'; // Portal
            }
            else if(board->board[idx].has_dot) {
                session->grid[idx] = '.'; // Dot
            }
            else {
                session->grid[idx] = ' '; // Empty
            }
        }
    }
    
    if (board->n_pacmans > 0) {
        session->score = board->pacmans[0].points;
        session->pacman_x = board->pacmans[0].pos_x;
        session->pacman_y = board->pacmans[0].pos_y;
    }
    if(board->pacmans[0].alive == 0) {
        session->game_over = 1;
    }

}

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;

    board_t *board = ghost_arg->board;
    GameSession *session = (GameSession*) ghost_arg->game_session;
    int ghost_ind = ghost_arg->ghost_index;
    ghost_t* ghost = &board->ghosts[ghost_ind];

    free(ghost_arg);

    while (1) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_wrlock(&board->state_lock);
        if (!session->active) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

void* send_board_thread(void* arg){
    pacman_thread_arg_t *pac_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pac_arg->board;
    GameSession *session = pac_arg->game_session;

    free(pac_arg);
    debug("Board sender thread starts now\n");

    while(session->active) {
        sleep_ms(100); // Dormir 100ms (10 FPS)

        pthread_mutex_lock(&session->lock);
        // 1. Prepara e envia o tabuleiro
        translate_board_to_session(board, session);
        send_board_to_client(session);
        debug("Board sent to client\n");

        pthread_mutex_unlock(&session->lock);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    open_debug_file("server_debug.log");
    if(argc != 4) {
        debug("Usage: %s <levels_dir(str)> <max_sessions(int)> <nome_FIFO_de_registo(str)>\n", argv[0]);
        return 1;
    }

    // 1. Cria o pipe do servidor
    char server_fifo[MAX_PIPE_PATH_LENGTH];
    strncpy(server_fifo, argv[3], MAX_PIPE_PATH_LENGTH);
    mkfifo(server_fifo, 0666);

    // 2. Abre para leitura
    int fd = open(server_fifo, O_RDONLY);
    if (fd == -1) {
        debug("Failed to open server FIFO\n");
        return 1;
    }
    
    // 3. Espera por conexão de cliente
    // Protocolo connect: OP(1) + PipeReq(40) + PipeNotif(40) + 2(\0) = 81 bytes
    char connectbuf[(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)];
    ssize_t n = read(fd, &connectbuf, sizeof(connectbuf));

    if (n <= 0) {
        debug("Failed to read connection request\n");
        close(fd);
        return 1;
    }

    char opcode = connectbuf[0];
    if (opcode == OP_CODE_CONNECT && n==(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)) {
        // 4. Inicializa sessão de jogo
        debug("Client connected\n");
        GameSession session;
        board_t game_board;
        memset(&session, 0, sizeof(session));
        session.active = 1;
        pthread_mutex_init(&session.lock, NULL);

        // 5. Abrir pipes do cliente, por ordem que são abertos no api.c
        char req_path[MAX_PIPE_PATH_LENGTH], notif_path[MAX_PIPE_PATH_LENGTH];
        memcpy(req_path, connectbuf + sizeof(char), MAX_PIPE_PATH_LENGTH);
        memcpy(notif_path, connectbuf + sizeof(char) + MAX_PIPE_PATH_LENGTH * sizeof(char), MAX_PIPE_PATH_LENGTH);

        session.fd_notif = open(notif_path, O_WRONLY);
        if (session.fd_notif == -1) {
            debug("Failed to open client request FIFO\n");
            close(fd);
            return 1;
        }

        session.fd_req = open(req_path, O_RDONLY);
        if (session.fd_req == -1) {
            debug("Failed to open client notification FIFO\n");
            close(session.fd_notif);
            close(fd);
            return 1;
        }
        debug("Client FIFOs opened\n");


        // 6. Enviar Ack de conexão
        char ack[2 * sizeof(char)] = {OP_CODE_CONNECT, 0}; // Result 0 = OK

        if(write(session.fd_notif, ack, 2 * sizeof(char)) == -1) {
            debug("Failed to send connection ACK to client\n");
            close(session.fd_req);
            close(session.fd_notif);
            close(fd);
            return 1;
        }
        debug("Connection ACK sent to client\n");

        int total_levels = 0;
        DIR* count_dir = opendir(argv[1]);
        struct dirent* count_entry;
        while ((count_entry = readdir(count_dir)) != NULL) {
            char *dot = strrchr(count_entry->d_name, '.');
            if (!dot) continue;
            if (strcmp(dot, ".lvl") == 0) {
                total_levels++;
            }
        }
        closedir(count_dir);

        // 7. Abre a diretoria

        DIR* level_dir = opendir(argv[1]);
        if (level_dir == NULL) {
            debug("Failed to open directory: %s\n", argv[1]);
            return 0;
        }

        struct dirent* entry;
        int accumulated_points = 0;
        int current_level_idx = 0;
        int result;
        while ((entry = readdir(level_dir)) != NULL && !session.game_over){
            if (entry->d_name[0] == '.') continue;

            char *dot = strrchr(entry->d_name, '.');
            if (!dot) continue;

            if (strcmp(dot, ".lvl") == 0) {
                // 8. Carregar nível
                if (load_level(&game_board, &session, entry->d_name, argv[1], accumulated_points) == -1) {
                    debug("Failed to load level: %s\n", entry->d_name);
                    continue;
                }
                current_level_idx++;

                while(1){
                    session.active = 1;

                    // 9. Criar threads para o jogo, cuidado com a ordem!
                    pthread_t input_tid, board_tid;
                    pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                    // 9.1 Criar thread de envio do tabuleiro
                    pacman_thread_arg_t *board_arg = malloc(sizeof(pacman_thread_arg_t));
                    board_arg->board = &game_board;
                    board_arg->game_session = &session;
                    pthread_create(&board_tid, NULL, send_board_thread, board_arg);

                    // 9.2 Criar threads dos fantasmas
                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                        arg->board = &game_board;
                        arg->ghost_index = i;
                        arg->game_session = &session;
                        pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                    }

                    // 9.3 Criar thread do input do pacman
                    pacman_thread_arg_t *pac_arg = malloc(sizeof(pacman_thread_arg_t));
                    pac_arg->board = &game_board;
                    pac_arg->game_session = &session;
                    pthread_create(&input_tid, NULL, input_handler_thread, (void *) pac_arg);

                    // 10. Esperar pela thread de input terminar
                    int *retval;
                    pthread_join(input_tid, (void**)&retval);

                    // 11. Parar o jogo
                    pthread_mutex_lock(&session.lock);
                    session.active = 0;
                    pthread_mutex_unlock(&session.lock);

                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        pthread_join(ghost_tids[i], NULL);
                    }
                    free(ghost_tids);

                    pthread_join(board_tid, NULL);

                    // 12. Processar o resultado do jogo
                    result = *retval;
                    free(retval);
                    if (result == NEXT_LEVEL) {
                        accumulated_points = game_board.pacmans[0].points;
                        if (current_level_idx == total_levels) {
                            // Último nível concluído
                            pthread_mutex_lock(&session.lock);
                            session.victory = 1;
                            translate_board_to_session(&game_board, &session);
                            send_board_to_client(&session);
                            pthread_mutex_unlock(&session.lock);
                            break; // Sair do loop de níveis
                        }
                        break; // Carregar próximo nível
                    }
                    if (result == QUIT_GAME) {
                        pthread_mutex_lock(&session.lock);
                        session.game_over = 1;
                        pthread_mutex_unlock(&session.lock);
                        break; // Sair do loop de níveis
                    }
                }
                if (current_level_idx != total_levels) {
                    unload_level(&game_board);
                    free(session.grid);
                }
            }
            
            if(session.game_over || session.victory) {
                break;
            }
        }

        char buf_quit[2 * sizeof(char)];
        int p = read(session.fd_req, &buf_quit, sizeof(buf_quit));
        debug("Read %d bytes from client request pipe before quitting\n", p);

        if (p == 2){
            // 13. Limpeza
            unload_level(&game_board);
            free(session.grid);
            close(fd);
            closedir(level_dir);
            pthread_mutex_destroy(&session.lock);
            unlink(server_fifo);   
            close_debug_file(); 
        }
    }

    return 0;
}
#define _POSIX_C_SOURCE 200809L
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
#include <semaphore.h>

#include "protocol.h"
#include "debug.h"
#include "server.h"

// VARIÁVEIS GLOBAIS --> tornar isto numa struct server_info_t 
sem_t server_semaphore;
pthread_mutex_t server_mutex;
char connectbuf[512][(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)]; // buffer para pedidos de conexão
int users_queue_count = 0; //TODO:check if correct

GameSession **active_sessions = NULL;

char level_files_dirpath[128];
int max_sessions;
char server_fifo[MAX_PIPE_PATH_LENGTH];

volatile sig_atomic_t sigusr1_recebido = 0;

// Estrutura auxiliar apenas para esta função
typedef struct {
    int id;    // Pode ser o indice ou socket_fd
    int score;
} ScoreEntry;

/*Função auxiliar para tratar o sinal SIGUSR1*/
void trata_sinal_usr1(int sinal) {
    if (sinal == SIGUSR1) {
        sigusr1_recebido = 1;
        // A lógica pesada fica no main loop.
    }
}

void gerar_top5_pontuacoes() {
    FILE *f = fopen("top5_scores.txt", "w");
    if (!f) return;

    // Array temporário na stack para ordenar (evita mexer no array global)
    ScoreEntry temp_list[max_sessions];
    int count = 0;

    // 1. Recolher apenas sessões ATIVAS
    // Nota: Lemos sem lock específico de sessão para não parar o jogo. 
    // Pode haver uma ligeira discrepância no score, mas é aceitável para "Live Stats".
    for (int i = 0; i < max_sessions; i++) {
        GameSession *s = active_sessions[i];
        // Verifica se ponteiro existe E se a sessão está marcada como ativa
        if (s != NULL && s->active) {
            temp_list[count].id = s->fd_req; // Usamos o FD como ID único
            temp_list[count].score = s->score;
            count++;
        }
    }

    // 2. Ordenar (Bubble Sort é rápido suficiente para < 100 elementos)
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (temp_list[j].score < temp_list[j + 1].score) {
                // Trocar
                ScoreEntry temp = temp_list[j];
                temp_list[j] = temp_list[j + 1];
                temp_list[j + 1] = temp;
            }
        }
    }

    // 3. Escrever no ficheiro
    fprintf(f, "--- TOP 5 JOGADORES (Total Ativos: %d) ---\n", count);
    int limite = (count < 5) ? count : 5;
    
    for (int i = 0; i < limite; i++) {
        fprintf(f, "%dº Lugar - ID: %d - Pontos: %d\n", 
                i + 1, temp_list[i].id, temp_list[i].score);
    }

    fclose(f);
    printf("Estatísticas geradas: %d jogadores listados.\n", count);
}

static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
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

        // 1. Verificar se o jogo acabou
        if (board->pacmans[0].alive == 0) {
            pthread_mutex_lock(&session->lock);
            session->active = 0;
            pthread_mutex_unlock(&session->lock);
            *retval = QUIT_GAME;
            break;
        }

        // 2. Ler (bloqueante, sem lock, o que é bom)
        memset(buf, 0, sizeof(buf));
        int n = read(session->fd_req, &buf, sizeof(buf));
        debug("Read %d bytes from client request pipe\n", n);

        if (n < 0) { //Erro
            debug("Error reading client request\n");
            pthread_mutex_lock(&session->lock);
            session->active = 0;
            pthread_mutex_unlock(&session->lock);
            break;
        }
        if (n == 0) { //Pipe fechado -> monstro matou pacman entre passos 1 e 2
            debug("Client request pipe closed\n");
            pthread_mutex_lock(&session->lock);
            session->active = 0;
            session->game_over = 1;
            pthread_mutex_unlock(&session->lock);
            *retval = QUIT_GAME;
            break;
        }

        // 3. Trancar Sessão para processar
        pthread_mutex_lock(&session->lock);
        
        char opcode = buf[0];

        // 4. Processar comando

        // 4.1 Se o cliente pediu para disconectar
        if (opcode == OP_CODE_DISCONNECT) {
            session->active = 0; // Parar as outras threads
            pthread_mutex_unlock(&session->lock); // Destrancar antes de sair
            *retval = QUIT_GAME;
            debug("Client requested disconnection\n");
            return (void*) retval;
        }
        
        // 4.2 Se o cliente enviou um comando de jogo
        if (opcode == OP_CODE_PLAY) {
            char command = buf[1];

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
                session->game_over = 1;
                //session->active = 0;
                pthread_mutex_unlock(&session->lock); // Destranca sessão
                break;
            }
        }

        // 6. Fim da iteração: destranca a sessão para a próxima volta
        pthread_mutex_unlock(&session->lock);
    }

    sleep_ms(100); // Pequena espera para garantir que o cliente recebe o estado final
    
    return (void*) retval;
}

/*Função para enviar o estado do tabuleiro para o cliente*/
int send_board_to_client(GameSession *session) {

    int header_size = 1*sizeof(char) + 6 * sizeof(int); // OP + 6 inteiros
    int board_size = session->width * session->height;

    debug("Preparing to send board update to client\n");
    char *buffer = malloc(header_size);
    if (!buffer) return 1;

    int p = 0;
    // 1. OP CODE
    buffer[p++] = OP_CODE_BOARD;

    // 2. Inteiros (memcpy para segurança binária)
    memcpy(buffer + p, &session->width, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->height, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->tempo, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->victory, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->game_over, sizeof(int)); p += sizeof(int);
    memcpy(buffer + p, &session->score, sizeof(int)); p += sizeof(int);

    // 3. Enviar cabeçalho primeiro
    debug("Sending board update to client:\n");
    int n = write(session->fd_notif, buffer, header_size);
    if (n <= 0) {
        session->active = 0;
        return 1;
    }
    
    // 4. Preparar e enviar o tabuleiro
    char *board_buffer = malloc(board_size);
    if (!board_buffer) {
        free(buffer);
        return 1;
    }
    memcpy(board_buffer, session->grid, board_size);

    debug("Sending board grid to client:\n");
    int k = write(session->fd_notif, board_buffer, board_size);

    if (k <= 0) {
        session->active = 0;
        return 1;
    }

    // 5. Limpar
    free(buffer);
    free(board_buffer);
    return 0;
}

/*Função para traduzir o estado na estrutura do tabuleiro para a estrutura de sessão*/
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

/*Tarefa responsável pelo movimento independente dos monstros*/
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
            break;
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
    return NULL;
}

/*Tarefa responsável pelo envio periódico do estado do tabuleiro para o cliente*/
void* send_board_thread(void* arg){
    pacman_thread_arg_t *pac_arg = (pacman_thread_arg_t*) arg;

    board_t *board = pac_arg->board;
    GameSession *session = pac_arg->game_session;

    free(pac_arg);
    debug("Board sender thread starts now\n");

    while(session->active) {
        sleep_ms(100); // Dormir 100ms (10 FPS)

        pthread_mutex_lock(&session->lock);
        if (!session->active) {
            pthread_mutex_unlock(&session->lock);
            break;
        }
        // 1. Prepara e envia o tabuleiro
        translate_board_to_session(board, session);
        send_board_to_client(session);
        debug("Board sent to client\n");

        pthread_mutex_unlock(&session->lock);
    }
    return NULL;
}

/*Tarefa responsável pelo jogo de cada cliente*/
void *session_thread(void *arg) {
    GameSession *session = (GameSession *)arg;

    while(1){
        // 1. Esperar por pedido de conexão
        sem_wait(&server_semaphore); // Esperar por slot disponível

        pthread_mutex_lock(&server_mutex);

        // 2. Tratar do pedido mais antigo
        char connect_request[(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)];
        memcpy(connect_request, connectbuf[0], sizeof(connect_request));

        // Deslocar os pedidos no buffer
        for (int i = 1; i < users_queue_count; i++) {
            memcpy(connectbuf[i - 1], connectbuf[i], sizeof(connectbuf[i]));
        }
        users_queue_count--;

        pthread_mutex_unlock(&server_mutex);

        // 3. Processar o pedido de conexão
        debug("Processing a new connection request in session thread\n");
        

        board_t game_board;
        memset(session, 0, sizeof(GameSession));
        memset(&game_board, 0, sizeof(game_board));
        session->active = 1;
        pthread_mutex_init(&session->lock, NULL);

        // 5. Abrir pipes do cliente, por ordem que são abertos no api.c
        char req_path[MAX_PIPE_PATH_LENGTH], notif_path[MAX_PIPE_PATH_LENGTH];
        memcpy(req_path, connect_request + sizeof(char), MAX_PIPE_PATH_LENGTH);
        memcpy(notif_path, connect_request + sizeof(char) + MAX_PIPE_PATH_LENGTH * sizeof(char), MAX_PIPE_PATH_LENGTH);

        debug("Notif pipe:%s\n", notif_path);
        session->fd_notif = open(notif_path, O_WRONLY);
        if (session->fd_notif == -1) {
            debug("Failed to open client request FIFO\n");
            return NULL;
        }

        session->fd_req = open(req_path, O_RDONLY);
        if (session->fd_req == -1) {
            debug("Failed to open client notification FIFO\n");
            close(session->fd_notif);
            return NULL;
        }
        debug("Client FIFOs opened\n");


        // 6. Enviar Ack de conexão
        char ack[2 * sizeof(char)] = {OP_CODE_CONNECT, 0}; // Result 0 = OK

        if(write(session->fd_notif, ack, 2 * sizeof(char)) == -1) {
            debug("Failed to send connection ACK to client\n");
            close(session->fd_req);
            close(session->fd_notif);
            return NULL;
        }
        debug("Connection ACK sent to client\n");

        int total_levels = 0;
        DIR* count_dir = opendir(level_files_dirpath);
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
        DIR* level_dir = opendir(level_files_dirpath);
        if (level_dir == NULL) {
            debug("Failed to open directory: %s\n", level_files_dirpath);
            return 0;
        }

        struct dirent* entry;
        int accumulated_points = 0;
        int current_level_idx = 0;
        int result;
        while ((entry = readdir(level_dir)) != NULL && !session->game_over){
            if (entry->d_name[0] == '.') continue;

            char *dot = strrchr(entry->d_name, '.');
            if (!dot) continue;

            if (strcmp(dot, ".lvl") == 0) {
                // 8. Carregar nível
                if (load_level(&game_board, session, entry->d_name, level_files_dirpath, accumulated_points) == -1) {
                    debug("Failed to load level: %s\n", entry->d_name);
                    continue;
                }
                current_level_idx++;

                while(1){
                    
                    session->active = 1;

                    // 9. Criar threads para o jogo, cuidado com a ordem!
                    pthread_t input_tid, board_tid;
                    pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));

                    // 9.1 Criar thread de envio do tabuleiro
                    pacman_thread_arg_t *board_arg = malloc(sizeof(pacman_thread_arg_t));
                    board_arg->board = &game_board;
                    board_arg->game_session = session;
                    pthread_create(&board_tid, NULL, send_board_thread, board_arg);

                    // 9.2 Criar threads dos fantasmas
                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                        arg->board = &game_board;
                        arg->ghost_index = i;
                        arg->game_session = session;
                        pthread_create(&ghost_tids[i], NULL, ghost_thread, (void*) arg);
                    }

                    // 9.3 Criar thread do input do pacman
                    pacman_thread_arg_t *pac_arg = malloc(sizeof(pacman_thread_arg_t));
                    pac_arg->board = &game_board;
                    pac_arg->game_session = session;
                    pthread_create(&input_tid, NULL, input_handler_thread, (void *) pac_arg);

                    // 10. Esperar pela thread de input terminar
                    int *retval;
                    pthread_join(input_tid, (void**)&retval);

                    // 11. Parar o jogo
                    pthread_mutex_lock(&session->lock);
                    session->active = 0;
                    pthread_mutex_unlock(&session->lock);

                    for (int i = 0; i < game_board.n_ghosts; i++) {
                        pthread_join(ghost_tids[i], NULL);
                    }

                    free(ghost_tids);

                    pthread_join(board_tid, NULL);

                    // 12. Processar o resultado do jogo
                    result = *retval;
                    free(retval);

                    // 12.1 Se o pacman chegou ao portal
                    if (result == NEXT_LEVEL) {
                        accumulated_points = game_board.pacmans[0].points;

                        // Último nível concluído
                        if (current_level_idx == total_levels) {
                            pthread_mutex_lock(&session->lock);
                            session->victory = 1;
                            pthread_mutex_unlock(&session->lock);
                            translate_board_to_session(&game_board, session);// tem que enviar o tab final de vitória aqui porque só aqui 
                            send_board_to_client(session);// é que ele sabe se os níveis acabaram
                            break; // Sair do loop de níveis
                        }

                        // Preparar para o próximo nível
                        if (current_level_idx != total_levels) {
                            unload_level(&game_board);
                            free(session->grid);
                            session->grid = NULL;
                        }
                        break; // Carregar próximo nível
                    }

                    // 12.2 Se o pacman morreu ou pediu para sair
                    if (result == QUIT_GAME) {
                        pthread_mutex_lock(&session->lock);
                        session->game_over = 1;
                        pthread_mutex_unlock(&session->lock);
                        unload_level(&game_board);
                        free(session->grid);
                        session->grid = NULL;
                        break; // Sair do loop de níveis
                    }
                } //fim do while do nível
            } //fim do if do .lvl

            // Se o jogo acabou, sair do loop dos níveis
            if(session->game_over || session->victory || result == QUIT_GAME) {
                break;
            }
        } //fim do while dos níveis

        // 13. Limpeza
        close(session->fd_req);
        close(session->fd_notif);

        closedir(level_dir);
        pthread_mutex_destroy(&session->lock);  
    }
    return NULL;
}

/*Tarefa responsável pelo atendimento aos pedidos de conexão dos clientes*/
void* host_thread(void* arg) {
    (void)arg;
    // --- MUDANÇA 2: A host_thread decide escutar o sinal ---
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &mask, NULL); // <--- AQUI!

    // 1. Criar pipe do servidor
    mkfifo(server_fifo, 0666);

    // 2. Abre para leitura
    int fd = open(server_fifo, O_RDONLY);

    // 2.1 Abrir para escrita dummy para não receber EOF
    int dummy_fd = open(server_fifo, O_WRONLY);

    if (fd == -1 || dummy_fd == -1) {
        debug("Failed to open server FIFO\n");
        return NULL;
    }

    // 3. Inicializa semáforo e buffer produtor-consumidor
    sem_init(&server_semaphore, 0, 0); // começa a zeros, o passo 4. trata da capacidade
    pthread_mutex_init(&server_mutex, NULL); //TODO: check for locks

    // 4. Inicializar threads de sessão de jogo (consumidores)
    for (int i = 0; i < max_sessions; i++) {
        pthread_t sessions;
        active_sessions[i] = malloc(sizeof(GameSession));
        pthread_create(&sessions, NULL, session_thread, (void*)active_sessions[i]);
    }

    char temp_buf[(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)];

    while(1){

        // VERIFICAÇÃO DA FLAG
        if (sigusr1_recebido) {
            printf("Sinal SIGUSR1 detetado. A gerar estatísticas...\n");
            gerar_top5_pontuacoes(); // Função que crias no passo 5
            sigusr1_recebido = 0;    // Reset da flag
        }

        // 4. Espera por conexão de cliente
        // Protocolo connect: OP(1) + PipeReq(40) + PipeNotif(40) + 2(\0) = 81 bytes
        memset(temp_buf, 0, sizeof(temp_buf));

        int n = read(fd, &temp_buf, sizeof(temp_buf));

        if (n < 0) {
            if (errno == EINTR) {
                continue; // Foi só o sinal, volta ao início do while para verificar a flag sigusr1_recebido
            }
            debug("Failed to read connection request\n");
            close(fd);
            return NULL;
        }
        if (n == 0) {
            continue; // Ignorar ou tratar desconexão do pipe
        }

        char opcode = temp_buf[0];
        if (opcode == OP_CODE_CONNECT && n == (1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)) {
            // 5. Coloca pedido na fila (buffer produtor-consumidor)
            pthread_mutex_lock(&server_mutex);

            memcpy(connectbuf[users_queue_count], temp_buf, sizeof(connectbuf[0]));
            sem_post(&server_semaphore);
            users_queue_count++;

            pthread_mutex_unlock(&server_mutex);
        }

    }

    // 6. Limpeza
    for (int i = 0; i < max_sessions; i++) {
        free(active_sessions[i]);
    }
    free(active_sessions);
    close(dummy_fd);
    unlink(server_fifo);
    close(fd);
    pthread_mutex_destroy(&server_mutex);
    sem_destroy(&server_semaphore);

    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGPIPE, SIG_IGN);

    // --- MUDANÇA 1: Bloquear SIGUSR1 no processo principal ---
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    // Isto garante que o main (e quem ele criar) ignora o sinal por defeito
    pthread_sigmask(SIG_BLOCK, &mask, NULL); 

    // Configurar o handler (podes manter isto aqui ou antes)
    struct sigaction sa;
    sa.sa_handler = trata_sinal_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; 
    sigaction(SIGUSR1, &sa, NULL);

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("Erro ao configurar SIGUSR1");
        return 1;
    }

    open_debug_file("server_debug.log");
    if(argc != 4) {
        debug("Usage: %s <levels_dir(str)> <max_sessions(int)> <nome_FIFO_de_registo(str)>\n", argv[0]);
        return 1;
    }

    strncpy(level_files_dirpath, argv[1], sizeof(level_files_dirpath) - 1);
    level_files_dirpath[sizeof(level_files_dirpath) - 1] = '\0';

    max_sessions = atoi(argv[2]);
    active_sessions = calloc(max_sessions, sizeof(GameSession*));

    if (active_sessions == NULL) {
        perror("Erro ao alocar memória para lista de sessões");
        return 1;
    }

    strncpy(server_fifo, argv[3], sizeof(server_fifo) - 1);
    server_fifo[sizeof(server_fifo) - 1] = '\0';

    pthread_t host_tid;
    pthread_create(&host_tid, NULL, host_thread, NULL);
    pthread_join(host_tid, NULL);

    close_debug_file();
    return 0;
}
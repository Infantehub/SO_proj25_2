#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#include "../include/protocol.h"

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

typedef enum {
    REACHED_PORTAL = 1,
    VALID_MOVE = 0,
    INVALID_MOVE = -1,
    DEAD_PACMAN = -2,
} move_t;

/*Função auxiliar para ver se está dentro dos limites*/
int is_valid_position(GameSession *s, int x, int y) {
    return (x >= 0 && x < s->width && y >= 0 && y < s->height);
}

// Só para inicializar o jogo (fictício)
void init_game(GameSession *s) {
    s->width = 5;
    s->height = 5;
    s->grid = malloc(s->width * s->height);
    s->tempo = 600; // Tempo inicial
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

/*Processa o comando do cliente e atualiza o estado do jogo
relativamente ao movimento do pacman*/
int process_command(GameSession *session, char command) {
    if(session->game_over) {
        return DEAD_PACMAN; // Jogo já acabou
    }
    int new_x = session->pacman_x;
    int new_y = session->pacman_y;

    // FIXME: como implemento a lógica do waiting?
    /*if (session->waiting > 0) {
        session->waiting -= 1;
        return VALID_MOVE;        
    }*/
   char direction = command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        /*case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;*/
        default:
            return INVALID_MOVE; // Invalid direction
    }

    if (!is_valid_position(session, new_x, new_y)) { 
        return INVALID_MOVE;
    }

    char target_content = session->grid[new_y * session->width + new_x];
    
    if (target_content == '@'){
        //FIXME: session->victory = 1; victory funciona quando acaba o nível ou todos os níveis?
        return REACHED_PORTAL;
    }
    if (target_content == 'W'){
        return INVALID_MOVE;
    }
    if (target_content == 'M'){
        session->grid[session->pacman_y * session->width + session->pacman_x] = ' '; // limpa a posição antiga
        session->game_over = 1;
        return DEAD_PACMAN;
    }
    if (target_content == '.'){
        session->score += 1; 
        session->grid[session->pacman_y * session->width + session->pacman_x] = ' '; // come um ponto
        session->pacman_x = new_x;
        session->pacman_y = new_y;
        session->grid[new_y * session->width + new_x] = 'P';
        return VALID_MOVE;
    }
    return INVALID_MOVE;
}

void* input_handler_thread(void* arg){
    GameSession *session = (GameSession*) arg;
    char buf[2];

    while(session->active) {
        // 1. Espera por ler comando do cliente
        ssize_t n = read(session->fd_req, &buf, sizeof(buf));
        if (n <= 0) {
            session->active = 0;
            break;
        }
        char opcode = buf[0];
        char command = buf[1];
        
        if (opcode == OP_CODE_DISCONNECT) {
            session->active = 0;
        }
        else if (opcode == OP_CODE_PLAY) {
            // 2. Processa comando de jogo
            pthread_mutex_lock(&session->lock);
            /*int result = */process_command(session, command);
            pthread_mutex_unlock(&session->lock);
        }
    }
    return NULL;
}
void send_board_to_client(GameSession *session) {
    int header_size = 1 + 6 * sizeof(int); // OP + 6 inteiros
    int board_size = session->width * session->height;
    int total_size = header_size + board_size;

    char *buffer = malloc(total_size);
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

    // 3. O Tabuleiro
    memcpy(buffer + p, session->grid, board_size);

    // 4. Enviar TUDO DE UMA VEZ
    write(session->fd_notif, buffer, total_size);

    free(buffer);
}

void* send_board_thread(void* arg){
    GameSession *session = (GameSession*) arg;

    while(session->active) {
        sleep(1); // Dormir 1s (1 FPS)

        pthread_mutex_lock(&session->lock);
        // 1. Prepara e envia o tabuleiro
        send_board_to_client(session);
        pthread_mutex_unlock(&session->lock);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    open_debug_file("server_debug.log");
    if(argc != 4) {
        debug("Usage: %s <levels_dir(str)> <max_games(int)> <nome_FIFO_de_registo(str)>\n", argv[0]);
        return 1;
    }

    // 1. Cria o pipe do servidor
    char server_fifo[MAX_PIPE_PATH_LENGTH] = "/pacman_server_fifo";
    mkfifo(server_fifo, 0666);

    // 2. Abre para leitura
    int fd = open(server_fifo, O_RDONLY);
    if (fd == -1) {
        debug("Failed to open server FIFO\n");
        return 1;
    }
    
    // 3. Espera por conexão de cliente
    // Protocolo connect: OP(1) + PipeReq(40) + PipeNotif(40) + 2(\0) = 83 bytes
    char connectbuf[1 + 2*MAX_PIPE_PATH_LENGTH + 2];
    ssize_t n = read(fd, &connectbuf, sizeof(connectbuf));

    if (n <= 0) {
        debug("Failed to read connection request\n");
        close(fd);
        return 1;
    }

    char opcode = connectbuf[0];
    if (opcode == OP_CODE_CONNECT && n==83){
        // 4. Inicializa sessão de jogo
        GameSession game;
        memset(&game, 0, sizeof(game));
        game.active = 1;
        pthread_mutex_init(&game.lock, NULL);

        // 5. Abrir pipes do cliente, por ordem que são criados!
        char req_path[MAX_PIPE_PATH_LENGTH], notif_path[MAX_PIPE_PATH_LENGTH];
        memcpy(req_path, connectbuf + 1, MAX_PIPE_PATH_LENGTH);
        memcpy(notif_path, connectbuf + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);

        game.fd_req = open(req_path, O_RDONLY);
        if (game.fd_req == -1) {
            debug("Failed to open client request FIFO\n");
            close(fd);
            return 1;
        }

        game.fd_notif = open(notif_path, O_WRONLY);
        if (game.fd_notif == -1) {
            debug("Failed to open client notification FIFO\n");
            close(game.fd_req);
            close(fd);
            return 1;
        }


        // 6. Enviar Ack de conexão
        char ack[2] = {OP_CODE_CONNECT, 0}; // Result 0 = OK

        if(write(game.fd_notif, ack, 2) == -1) {
            debug("Failed to send connection ACK to client\n");
            close(game.fd_req);
            close(game.fd_notif);
            close(fd);
            return 1;
        }

        // 7. Inicializar Jogo
        init_game(&game); // TODO: Função fictícia para inicializar o jogo

        // 8. Criar threads de input e envio de tabuleiro
        pthread_t input_thread, board_thread;
        pthread_create(&input_thread, NULL, input_handler_thread, &game);
        pthread_create(&board_thread, NULL, send_board_thread, &game);

        // 9. Esperar pelas threads terminarem
        pthread_join(input_thread, NULL);
        game.active = 0; // Sinaliza para a thread de tabuleiro parar
        pthread_join(board_thread, NULL);

        // 10. Limpeza
        close(game.fd_notif);
        close(game.fd_req);
        pthread_mutex_destroy(&game.lock);
        free(game.grid);
        unlink(server_fifo);   
        close_debug_file(); 
    }
    return 0;
}
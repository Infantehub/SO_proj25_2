#include "../../include/api.h"
#include "../../include/protocol.h"
#include "../../include/debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>

#define BOARD_HEADER_SIZE (1 + 6 * sizeof(int))

struct Session {
  char op_code;
  int fd_req_pipe;
  int fd_notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.op_code = -1};

static int read_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = read(fd, (char *)buf + total, len - total);
        if (n <= 0) return -1; 
        total += n;
    }
    return 1;
}

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
    debug("Connecting to server...\n");

    // 1. Criar os pipes do cliente
    if (mkfifo(req_pipe_path, 0666) == -1 ) {
        debug("Failed to create client request FIFO\n");
        return 1;
    }
    if (mkfifo(notif_pipe_path, 0666) == -1 ) {
        debug("Failed to create client notification FIFO\n");
        unlink(req_pipe_path);
        return 1;
    }
  
    debug("Client FIFOs created: %s, %s\n", req_pipe_path, notif_pipe_path);

    //2. Preparar estrutura de dados para pedido de conexão
    strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    char connect_req_buffer[(1 + 2 * MAX_PIPE_PATH_LENGTH) * sizeof(char)];
    memset(connect_req_buffer, 0, sizeof(connect_req_buffer));

    connect_req_buffer[0] = OP_CODE_CONNECT;
    strncpy(&connect_req_buffer[sizeof(char)], req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(&connect_req_buffer[sizeof(char) + MAX_PIPE_PATH_LENGTH * sizeof(char)], notif_pipe_path, MAX_PIPE_PATH_LENGTH);

    // 3. Abrir o FIFO para pedido de conexão ao servidor
    int fd_server = open(server_pipe_path, O_WRONLY);
    if (fd_server == -1) {
        debug("Failed to open server FIFO\n");
        return 1;
    }

    // 4. Enviar pedido de conexão
    if (write(fd_server, connect_req_buffer, sizeof(connect_req_buffer)) == -1) {
        debug("Failed to write connection request to server\n");
        return 1;
    }
    debug("Connection request sent to server\n");

    //5. Fechar o pipe do servidor, já que não é mais necessário
    if (close(fd_server) == -1) {
        debug("Failed to close server FIFO\n");
        return 1;
    }

    // 6. Abrir o pipe de notificações e requests sincronizado com o Servidor
    session.fd_notif_pipe = open(notif_pipe_path, O_RDONLY);
    session.fd_req_pipe = open(session.req_pipe_path, O_WRONLY);

    if (session.fd_notif_pipe == -1) {
        debug("Failed to open notification FIFO\n");
        return 1;
    }
    if (session.fd_req_pipe == -1) {
        debug("Failed to open request FIFO\n");
        return 1;
    }


    // 7. Ler a resposta de conexão (ack)
    char connect_resp_buffer[2 * sizeof(char)];

    if(read_all(session.fd_notif_pipe, connect_resp_buffer, sizeof(connect_resp_buffer)) == -1) {
        return 1;
    }
    char op, result;
    memcpy(&op, &connect_resp_buffer[0], sizeof(char));
    memcpy(&result, &connect_resp_buffer[1], sizeof(char));

    if (op == OP_CODE_CONNECT && result == 0) {
        // 8. Sucesso! Agora abrimos o pipe de pedidos para enviar jogadas futuras
        debug("Connected to server successfully\n");
        return 0;
    }

    debug("Connection to server failed\n");
    return 1; // Falha na conexão
}

void pacman_play(char command) {

    // 1. Preparar estrutura de dados para pedido de jogada
    char play_req_buffer[2 * sizeof(char)];
    memset(play_req_buffer, 0, sizeof(play_req_buffer));

    play_req_buffer[0] = OP_CODE_PLAY;
    play_req_buffer[1] = command;

    // 2. Enviar pedido de jogada ao servidor
    if (write(session.fd_req_pipe, &play_req_buffer, sizeof(play_req_buffer)) == -1) {
        debug("Failed to write play request to server\n");
    }
}

int pacman_disconnect() {
    //1. Enviar aviso de desconexão ao servidor

    char exit_buf[2 * sizeof(char)];
    exit_buf[0] = OP_CODE_DISCONNECT;
    exit_buf[1] = 0; //Just padding

    debug("Disconnecting from server...\n");
    if (write(session.fd_req_pipe, &exit_buf, sizeof(exit_buf)) == -1) {
        debug("Failed to write disconnect request to server\n");
        return 1;
    }
    debug("Disconnect request sent to server\n");

    //2. Fechar os pipes do cliente
    if (close(session.fd_req_pipe) == -1) {
        debug("Failed to close request pipe\n");
        return 1;
    }
    if (close(session.fd_notif_pipe) == -1) {
        debug("Failed to close notification pipe\n");
        return 1;
    }
    debug("Client pipes closed\n");

    //3. Apagar os pipes do cliente
    if (unlink(session.req_pipe_path) == -1) {
        debug("Failed to unlink request pipe\n");
    }
    if (unlink(session.notif_pipe_path) == -1) {
        debug("Failed to unlink notification pipe\n");
    }
    debug("Client pipes unlinked\n");
    close_debug_file();
    return 0;
}

Board receive_board_update(void) {
    Board board;
    char header[BOARD_HEADER_SIZE]; // Buffer para a string de texto

    memset(&board, 0, sizeof(Board));

    // 1. Ler a atualização do tabuleiro do pipe de notificações
    if(read_all(session.fd_notif_pipe, header, sizeof(header)) == -1) {
        return board;
    }
    
    // 2. Processar a atualização do tabuleiro
    int off = 0;
    char op = header[off++];
    if (op != OP_CODE_BOARD) {
        debug("Invalid OP code: %d\n", op);
        return board;
    }

    memcpy(&board.width, header + off, sizeof(int)); off += sizeof(int);
    memcpy(&board.height, header + off, sizeof(int)); off += sizeof(int);
    memcpy(&board.tempo, header + off, sizeof(int)); off += sizeof(int);
    memcpy(&board.victory, header + off, sizeof(int)); off += sizeof(int);
    memcpy(&board.game_over, header + off, sizeof(int)); off += sizeof(int);
    memcpy(&board.accumulated_points, header + off, sizeof(int));

    

    // 3. Alocar memória para os dados do tabuleiro
    board.data = (char*)malloc(board.width * board.height * sizeof(char));
    if (!board.data) {
        debug("Failed to allocate memory for board data\n");
        return board;
    }

    // 4. Ler os dados do tabuleiro do pipe de notificações
    if (read_all(session.fd_notif_pipe, board.data, board.width * board.height * sizeof(char)) == -1) {
        free(board.data);
        board.data = NULL;
        return board;
    }

    return board;
}
#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  char op_code;
  int fd_req_pipe;
  int fd_notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.op_code = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  open_debug_file("client-api-debug.log");
  debug("Connecting to server...\n");

  // 1. Criar os pipes do cliente
  if (mkfifo(req_pipe_path, 0666) == -1 || mkfifo(notif_pipe_path, 0666) == -1) {
      debug("Failed to create client FIFOs\n");
      return 1;
  }
  
  //2. Preparar estrutura de dados para pedido de conexão
  char connect_req_buffer[(1 + 2 * MAX_PIPE_PATH_LENGTH + 2) * sizeof(char)];
  memset(connect_req_buffer, 0, sizeof(connect_req_buffer));

  connect_req_buffer[0] = OP_CODE_CONNECT;
  strncpy(&connect_req_buffer[1], req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(&connect_req_buffer[1 + MAX_PIPE_PATH_LENGTH], notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  // 3. Abrir o FIFO para pedido de conexão ao servidor
  int fd_server = open(server_pipe_path, O_WRONLY);
  if (fd_server == -1) {
      debug("Failed to open server FIFO\n");
      return 1;
  }

  // 4. Enviar pedido de conexão
  if (write(fd_server, &connect_req_buffer, sizeof(connect_req_buffer)) == -1) {
      debug("Failed to write connection request to server\n");
      return 1;
  }

  //5. Fechar o pipe do servidor, já que não é mais necessário
  if (close(fd_server) == -1) {
      debug("Failed to close server FIFO\n");
      return 1;
  }

  // 6. Abrir o pipe de notificações do Servidor
  session.fd_notif_pipe = open(notif_pipe_path, O_RDONLY);

  if (session.fd_notif_pipe == -1) {
      debug("Failed to open notification FIFO\n");
      return 1;
  }

  // 7. Ler a resposta de conexão
  char connect_resp_buffer[2 * sizeof(char)];
  memset(connect_resp_buffer, 0, sizeof(connect_resp_buffer));
  read(session.fd_notif_pipe, &connect_resp_buffer, sizeof(connect_resp_buffer));    

  if (connect_resp_buffer[0] == OP_CODE_CONNECT && connect_resp_buffer[1] == 0) {
      // 8. Sucesso! Agora abrimos o pipe de pedidos para enviar jogadas futuras
      session.fd_req_pipe = open(session.req_pipe_path, O_WRONLY);
      return 0;
  }

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
  //1. Enviar pedido de desconexão ao servidor
  char op_code = OP_CODE_DISCONNECT;
  if (write(session.fd_req_pipe, &op_code, sizeof(op_code)) == -1) {
      debug("Failed to write disconnect request to server\n");
      return 1;
  }

  //2. Fechar os pipes do cliente
  if (close(session.fd_req_pipe) == -1) {
      debug("Failed to close request pipe\n");
      return 1;
  }
  if (close(session.fd_notif_pipe) == -1) {
      debug("Failed to close notification pipe\n");
      return 1;
  }

  //3. Apagar os pipes do cliente
  if (unlink(session.req_pipe_path) == -1) {
      debug("Failed to unlink request pipe\n");
  }
  if (unlink(session.notif_pipe_path) == -1) {
      debug("Failed to unlink notification pipe\n");
  }
  close_debug_file();
  return 0;
}

Board receive_board_update(void) {
  Board board;
  char buffer1[sizeof(char) + 6*sizeof(int)];

  // 1. Ler a atualização do tabuleiro do pipe de notificações
  if (read(session.fd_notif_pipe, &buffer1, sizeof(buffer1)) == -1) {
      debug("Failed to read board update from server\n");
  }

  // 2. Processar a atualização do tabuleiro
  if (buffer1[0] != OP_CODE_BOARD) {
      debug("Invalid op_code in board update\n");
      return;
  }
  memcpy(&board.width, &buffer1[1], sizeof(int));
  memcpy(&board.height, &buffer1[1 + sizeof(int)], sizeof(int));
  memcpy(&board.tempo, &buffer1[1 + 2*sizeof(int)], sizeof(int));
  memcpy(&board.victory, &buffer1[1 + 3*sizeof(int)], sizeof(int));
  memcpy(&board.game_over, &buffer1[1 + 4*sizeof(int)], sizeof(int));
  memcpy(&board.accumulated_points, &buffer1[1 + 5*sizeof(int)], sizeof(int));

  char *data_buffer = (char *)malloc(board.width * board.height * sizeof(char));
  if (data_buffer == NULL) {
      debug("Failed to allocate memory for board data\n");
      return;
  }
  if (read(session.fd_notif_pipe, data_buffer, board.width * board.height * sizeof(char)) == -1) {
      debug("Failed to read board data from server\n");
      free(data_buffer);
      return;
  }
  memcpy(board.data, data_buffer, board.width * board.height * sizeof(char));
  free(data_buffer);

  return board;
}

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int main() {
    char *server_fifo = "pacman_server_fifo";
    
    // 1. Cria o pipe do servidor
    mkfifo(server_fifo, 0666);
    printf("Dummy Server à escuta em %s...\n", server_fifo);

    // 2. Abre para leitura
    int fd = open(server_fifo, O_RDONLY);
    
    connect_req_t req;
    // 3. Lê o pedido de conexão
    read(fd, &req, sizeof(req));
    
    printf("Recebi pedido!\nPipe Pedidos: %s\nPipe Notif: %s\n", req.req_pipe, req.notif_pipe);

    // 4. Envia resposta de SUCESSO
    int fd_cli_notif = open(req.notif_pipe, O_WRONLY);
    connect_resp_t resp = {OP_CONNECT, 0};
    write(fd_cli_notif, &resp, sizeof(resp));
    printf("Enviei confirmação. Agora vou ler uma tecla...\n");

    // 5. Tenta ler uma tecla (simula o jogo a começar)
    int fd_cli_req = open(req.req_pipe, O_RDONLY);
    play_req_t play;
    read(fd_cli_req, &play, sizeof(play));
    printf("O cliente carregou na tecla: %c\n", play.command);

    // Limpeza
    close(fd_cli_notif);
    close(fd_cli_req);
    unlink(server_fifo);
    return 0;
}
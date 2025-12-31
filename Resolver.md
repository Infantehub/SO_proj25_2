Filosofia:
    -dentro do server, devo trabalhar com um GameSession e um board para cada cliente ou inserir a estrutura 
    board dentro da session?
        na primeira opção, o código torna-se mais chato, propenso a erros, mais cheio. Mas por outro lado, mais lógico
        E que tal se eu apenas traduzir para GameSession, antes da grid ser enviada? parecce-me bem... Assim, faço a lógica toda apenas mexendo no board e quando for enviar, mando a grid da game_session.
    -devo dar load ao jogo antes ou depois dos clientes se conectarem ao server?

A resolver:
    -por o cliente a ler os ficheiros .p (reutilizar a função read_pacman definida no Server_base)
    -lógica de fim de jogo e o seu respetivo draw board
    -verificar lógica de passagem de nível
    -porque é que o pacman aparece a branco?
    -a main do cliente não está a chegar ao fim, prova-> os pipes não estão a ser apagados
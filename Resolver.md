*Filosofia:*
    -dentro do server, devo trabalhar com um GameSession e um board para cada cliente ou inserir a estrutura 
    board dentro da session?
        na primeira opção, o código torna-se mais chato, propenso a erros, mais cheio. Mas por outro lado, mais lógico
        E que tal se eu apenas traduzir para GameSession, antes da grid ser enviada? parecce-me bem... Assim, faço a lógica toda apenas mexendo no board e quando for enviar, mando a grid da game_session. -->resolvido assim
    
    -devo dar load ao jogo antes ou depois dos clientes se conectarem ao server?
    -qual faz mais sentido começar primeir, a receiver thread ou a do ncurses?

*A resolver:*
    -por o cliente a ler os ficheiros .p (reutilizar a função read_pacman definida no Server_base)
    -verificar lógica de passagem de nível
    -verificar tamanho dos paths dos pipes com e sem /0

*Estética e sentido:*
    -as funções devem retornar 1 se der erro ou 0 se der certo, caso exceçoes
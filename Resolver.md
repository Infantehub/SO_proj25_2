*Filosofia:*
    -dentro do server, devo trabalhar com um GameSession e um board para cada cliente ou inserir a estrutura 
    board dentro da session?
        na primeira opção, o código torna-se mais chato, propenso a erros, mais cheio. Mas por outro lado, mais lógico
        E que tal se eu apenas traduzir para GameSession, antes da grid ser enviada? parecce-me bem... Assim, faço a lógica toda apenas mexendo no board e quando for enviar, mando a grid da game_session. -->resolvido assim
    
    -devo dar load ao jogo antes ou depois dos clientes se conectarem ao server?
    -qual faz mais sentido começar primeir, a receiver thread ou a do ncurses?

*A resolver:*
    -verificar tamanho dos paths dos pipes com e sem /0
    -se ele não conseguir dar open, qual o sinal que deve ignorar para reotrnar -1?
    -lógica de contagem de níveis é overkill? será que dá para simplificar isso?
    -Nunca uso o result CONTINUE_PLAY, devo usar ou não preciso?
    -A variável game_over está com um uso estranho... game over acontece só queando perde ou também quando ganha? quando deve ser ativada?
    -rever os sleeps e os tempos

*Estética e sentido:*
    -as funções devem retornar 1 se der erro ou 0 se der certo, caso exceçoes
    -escrever breve descrição para cada função, bem como os seus passos(//1. etc)
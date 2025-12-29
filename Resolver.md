Filosofia:
    -dentro do server, devo trabalhar com um GameSession e um board para cada cliente ou inserir a estrutura 
    board dentro da session?
        na primeira opção, o código torna-se mais chato, propenso a erros, mais cheio. Mas por outro lado, mais lógico
        E que tal se eu apenas traduzir para GameSession, antes da grid ser enviada? parecce-me bem... Assim, faço a lógica toda apenas mexendo no board e quando for enviar, mando a grid da game_session.

A resolver:
    -como funciona o display do ncurses para o cliente?
         acho que tenho de criar uma thread de display
    
Próximo passo:
    -cliente receber o board correto
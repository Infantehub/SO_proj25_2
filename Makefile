# 1. Defina as suas subpastas aqui
SUBDIRS = Client_base Server_base

# 2. Define que estes alvos não são arquivos reais
.PHONY: all clean $(SUBDIRS)

# 3. Regra padrão: chama o make em cada subpasta
all: $(SUBDIRS)

# 4. Entra na pasta e executa o make
$(SUBDIRS):
	$(MAKE) -C $@

# 5. Regra para limpar (clean) em todas as pastas
clean:
	for dir in $(SUBDIRS); do \
		$(MAKE) -C $$dir clean; \
	done
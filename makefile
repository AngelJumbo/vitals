PREFIX = /usr/local

vitals: vitals.c cpu.c ram.c utils.c network.c disk.c
	$(CC) -lpthread vitals.c cpu.c ram.c utils.c network.c disk.c -o vitals

debug: vitals.c cpu.c ram.c utils.c network.c disk.c
	$(CC) -Wall -lpthread vitals.c cpu.c ram.c utils.c network.c disk.c -g -o vitals

.PHONY: clean
clean:
	$(RM) vitals

.PHONY: install
install: vitals
	install 				-D vitals 		$(DESTDIR)$(PREFIX)/bin/vitals

.PHONY: unistall
unistall:
	$(RM) $(DESTDIR)$(PREFIX)/bin/vitals

PREFIX = /usr/local

vitals: vitals.c cpu.c ram.c utils.c network.c
	$(CC) vitals.c cpu.c ram.c utils.c network.c -o vitals

debug: vitals.c cpu.c ram.c utils.c network.c
	$(CC) vitals.c cpu.c ram.c utils.c network.c -g -o vitals

.PHONY: clean
clean:
	$(RM) vitals

.PHONY: install
install: vitals
	install 				-D vitals 		$(DESTDIR)$(PREFIX)/bin/vitals

.PHONY: unistall
unistall:
	$(RM) $(DESTDIR)$(PREFIX)/bin/vitals

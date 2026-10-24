# Host-side unit tests for components/ddns/ddns_core.c (no ESP-IDF needed).
#   make -C tools/ddns_host_test test
CORE := ../../components/ddns
CFLAGS := -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(CORE)

test_ddns_core: test_ddns_core.c $(CORE)/ddns_core.c $(CORE)/ddns_core.h
	$(CC) $(CFLAGS) -o $@ test_ddns_core.c $(CORE)/ddns_core.c

.PHONY: test clean
test: test_ddns_core
	./test_ddns_core

clean:
	rm -f test_ddns_core

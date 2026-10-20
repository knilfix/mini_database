build/mini_database: mini_database.c
	@mkdir -p build
	gcc -Wall -Wextra -g -o $@ $<
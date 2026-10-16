build/mini_database: mini_database.c
	@mkdir -p build
	cc -Wall -Wextra -g -o $@ $<
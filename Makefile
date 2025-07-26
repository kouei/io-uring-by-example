C_STD := -std=gnu11
CC_FLAG := -g -O0 -Wall -luring $(C_STD)

main: main.c
	gcc $^ $(CC_FLAG) -o $@

.PHONY: clang-tidy clang-format clean

clang-tidy:
	clang-tidy ./main.c -- $(C_STD)

clang-format:
	clang-format -i ./main.c

clean:
	rm -f main
C_STD := -std=gnu11
CC_FLAG := -O2 -Wall -luring $(C_STD)

main: main.c
	gcc $^ -o $@ $(CC_FLAG)

.PHONY: clean clang-tidy

clang-tidy:
	clang-tidy ./main.c -- $(C_STD)

clang-format:
	clang-format -i ./main.c

clean:
	rm -f main
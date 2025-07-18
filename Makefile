C_STD := -std=c11
CC_FLAG := -O2 -Wall -luring $(C_STD)

main: main.c
	gcc $^ -o $@ $(CC_FLAG)

.PHONY: clean clang-tidy

clang-tidy:
	clang-tidy ./main.c -- $(C_STD)

clean:
	rm -f main
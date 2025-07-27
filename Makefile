C_STD := -std=gnu11
CC_FLAG := -g -O0 -Wall -luring $(C_STD)
SRC := main.c

main: $(SRC)
	gcc $^ $(CC_FLAG) -o $@

.PHONY: clang-tidy clang-format clean

clang-tidy:
	clang-tidy $(SRC) -- $(C_STD)

clang-format:
	clang-format -i $(SRC)

clean:
	rm -f main
CC_FLAG := -O2 -Wall -luring

main: main.c
	gcc $^ -o $@ $(CC_FLAG)

.PHONY: clean
clean:
	rm -f main
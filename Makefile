SRCS += $(shell find . -maxdepth 1 -name "*.c")
lib:
	$(CC) -O2 -fPIC -shared $(SRCS) -o libmyalloc.so
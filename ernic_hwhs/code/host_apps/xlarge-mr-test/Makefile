CFLAGS =-g
all: xlarge_mr_test.c
	gcc -g xlarge_mr_test.c -o xlarge_mr_test -lrdmacm -libverbs -lpthread
clean:
	@rm -f xlarge_mr_test

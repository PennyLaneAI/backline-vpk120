CFLAGS =-g
all: inv_imm_test_in.c inv_imm_test_out.c
	gcc -g inv_imm_test_in.c -o inv_imm_test_in -lrdmacm -libverbs -lpthread
	gcc -g inv_imm_test_out.c -o inv_imm_test_out -lrdmacm -libverbs -lpthread
clean:
	@rm -f inv_imm_test_in inv_imm_test_out

NAME := umm
LIBSOURCES =*.c
OBJS_BIN = *.o
MAJOR = 1
MINOR = 0
VERSION = $(MAJOR).$(MINOR)

all: lib$(NAME).so

lib$(NAME).so.$(VERSION): $(OBJS_BIN)
	$(CC) $(LDFLAGS) $(OBJS_BIN) -shared -Wl,-soname,lib$(NAME).so.$(MAJOR) -o lib$(NAME).so.$(VERSION)

lib$(NAME).so: lib$(NAME).so.$(VERSION)
	rm -f lib$(NAME).so lib$(NAME).so.$(MAJOR)
	ln -s lib$(NAME).so.$(VERSION) lib$(NAME).so.$(MAJOR)
	ln -s lib$(NAME).so.$(MAJOR) lib$(NAME).so
%.o: %.c
	$(CC) $(CFLAGS) -c -fPIC $(LIBSOURCES)

clean:
	rm -rf *.o *.so *.so.*

FS=/usr/local/freeswitch
#FS_SRC=/usr/src/voip/freeswitch
FS_SRC=/usr/local/src/aicc/freeswitch
CC=cc -g -fPIC -O3 -Wall
MAKE=make
INCLUDE=-I$(FS_SRC)/src/include -I$(FS_SRC)/libs/libteletone/src
SUBDIRS=g729a_v11

all : mod_g729.o subdirs
	$(CC) $(INCLUDE) -o mod_g729.so -Wl,-x mod_g729.o  -lm -L./g729a_v11 -lg729 -L/usr/local/lib  \
	-shared $(FS)/lib/libfreeswitch.so  -Wl,--rpath -Wl,$(FS)/lib  


mod_g729.o: mod_g729.c
	$(CC) -c mod_g729.c $(INCLUDE)

subdirs:
	for dir in $(SUBDIRS); do \
    $(MAKE) -C $$dir; \
	done

clean:
	rm *.o mod_g729.so
	for dir in $(SUBDIRS); do \
    $(MAKE) -C $$dir clean; \
	done


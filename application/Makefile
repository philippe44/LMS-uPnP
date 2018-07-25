BASE		= ..
OS 			:= $(shell uname)

ifeq ($(OS),Darwin)
LINKSTATIC	=
else
LINKSTATIC 	= -static
endif

TOOLS		= ./tools
SRC			= ./squeeze2upnp
SQUEEZETINY	= ./squeezetiny

VALGRIND	= $(BASE)/valgrind
UPNP 		= $(BASE)/libupnp/1.6.19
FETCHER		= $(BASE)/http_fetcher

DEFINES 	= -D_FILE_OFFSET_BITS=64 -DRESAMPLE 
CFLAGS 		+= -fdata-sections -ffunction-sections 

vpath %.c $(TOOLS):$(SRC):$(SQUEEZETINY)

INCLUDE = -I. \
		  -I$(VALGRIND)/memcheck -I$(VALGRIND)/include \
		  -I$(SQUEEZETINY) \
		  -I$(TOOLS) \
		  -I$(SRC) -I$(SRC)/inc \
		  -I$(BASE)/flac/include\
		  -I$(BASE)/faad2/include\
		  -I$(BASE)/libmad\
		  -I$(BASE)/libvorbis/include -I$(BASE)/libogg/include\
		  -I$(BASE)/soxr/src\
		  -I$(UPNP)/ixml/inc -I$(UPNP)/upnp/inc -I$(UPNP)/build/inc -I$(UPNP)/threadutil/inc 
		  		  
DEPS	= $(SQUEEZETINY)/squeezedefs.h
				  
SOURCES = 	slimproto.c buffer.c util.c output_http.c main.c \
			stream.c decode.c pcm.c flac_thru.c thru.c process.c resample.c \
			util_common.c avt_util.c mr_util.c tinyutils.c squeeze2upnp.c \
			log_util.c config_upnp.c
		
SOURCES_LIBS = flac.c mad.c faad.c vorbis.c output.c
		
OBJECTS 		= $(patsubst %.c,$(OBJ)/%.o,$(SOURCES)) $(patsubst %.c,$(OBJ)/%.o,$(SOURCES_LIBS)) 
OBJECTS_STATIC 	= $(patsubst %.c,$(OBJ)/%.o,$(SOURCES)) $(patsubst %.c,$(OBJ)/%-static.o,$(SOURCES_LIBS)) 

LIBRARY 	= $(OBJ)/libupnp.a $(OBJ)/libixml.a $(OBJ)/libthreadutil.a $(OBJ)/libsoxr.a
LIBRARY_STATIC 	= $(LIBRARY) $(OBJ)/libfaad.a $(OBJ)/libFLAC.a $(OBJ)/libmad.a $(OBJ)/libvorbisfile.a $(OBJ)/libvorbis.a $(OBJ)/libogg.a 

all: $(EXECUTABLE) $(EXECUTABLE_STATIC)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -o $@

$(EXECUTABLE_STATIC): $(OBJECTS_STATIC)
	$(CC) $(OBJECTS_STATIC) $(LIBRARY_STATIC) $(LDFLAGS) $(LINKSTATIC) -o $@	

$(OBJECTS): $(DEPS) | bin $(OBJ)

$(OBJ):
	@mkdir -p $@
	
bin:	
	@mkdir -p bin

$(OBJ)/%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

$(OBJ)/%-static.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DLINKALL $(INCLUDE) $< -c -o $(OBJ)/$*-static.o	
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(OBJECTS_STATIC) $(EXECUTABLE_STATIC) 


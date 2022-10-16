ifeq ($(CC),cc)
CC=$(lastword $(subst /, ,$(shell readlink -f `which cc`)))
endif

PLATFORM ?= $(firstword $(subst -, ,$(CC)))
HOST ?= $(word 2, $(subst -, ,$(CC)))

ifeq ($(OS),Darwin)
LINKSTATIC	=
LDFLAGS 	+= -Wl,-dead_strip
else
LINKSTATIC 	= -static
LDFLAGS 	+= -Wl,--gc-sections
endif

BUILDDIR	= build/$(PLATFORM)
EXECUTABLE 	= bin/squeeze2upnp-$(PLATFORM)
EXECUTABLE_STATIC = bin/squeeze2upnp-$(PLATFORM)-static

SRC			= squeeze2upnp
SQUEEZELITE	= squeezelite
TOOLS		= crosstools/src
MDNS		= libmdns/targets
PUPNP 		= libpupnp/targets/$(HOST)/$(PLATFORM)
CODECS		= libcodecs/targets
OPENSSL		= libopenssl/targets/$(HOST)/$(PLATFORM)
#VALGRIND	= $(BASE)/valgrind

#DEFINES 	= -D_FILE_OFFSET_BITS=64 -DPB_FIELD_16BIT -DHAVE_STDINT_H -DRESAMPLE -DCODECS -DUSE_SSL -D_GNU_SOURCE -DUPNP_STATIC_LIB
DEFINES 	= -DCODECS -DUSE_SSL -D_GNU_SOURCE -DUPNP_STATIC_LIB -DLINKALL

CFLAGS  	+= -Wall -Wno-deprecated-declarations -Wno-format-truncation -Wno-stringop-truncation -fPIC -ggdb -O2 $(DEFINES) -fdata-sections -ffunction-sections 
LDFLAGS += -s -lpthread -ldl -lm -lrt -lstdc++ -L. 

vpath %.c $(TOOLS):$(SRC):$(SQUEEZELITE)
vpath %.cpp $(TOOLS):$(SRC):$(SQUEEZELITE)

INCLUDE = -I$(OPENSSL)/include \
		  -I$(TOOLS) \
		  -I$(SQUEEZELITE) \
		  -I$(SRC)/inc \
		  -I$(PUPNP)/include/upnp -I$(PUPNP)/include/ixml -I$(PUPNP)/include/addons \
		  -I$(CODECS)/include/flac -I$(CODECS)/include/shine -I$(CODECS)/include/addons \
		  -I$(MDNS)/include/tinysvcmdns -I$(MDNS)/include/mdnssd
		  		  		  
DEPS	= $(SRC)/inc/squeezedefs.h
				  
SOURCES = slimproto.c buffer.c output_http.c output.c main.c \
		  stream.c decode.c pcm.c resample.c process.c alac.c flac.c \
		  flac_thru.c m4a_thru.c thru.c \
		  utils.c metadata.c mimetypes.c \
		  cross_util.c cross_log.c cross_net.c cross_thread.c platform.c \
		  config_upnp.c avt_util.c mr_util.c squeeze2upnp.c

SOURCES_LIBS = cross_ssl.c
		
OBJECTS	= $(patsubst %.c,$(BUILDDIR)/%.o,$(filter %.c,$(SOURCES) $(SOURCES_LIBS)))
OBJECTS	+= $(patsubst %.c,$(BUILDDIR)/%.o,$(filter %.cpp,$(SOURCES) $(SOURCES_LIBS)))
OBJECTS_STATIC = $(patsubst %.c,$(BUILDDIR)/%.o,$(filter %.c,$(SOURCES))) $(patsubst %.c,$(BUILDDIR)/%-static.o,$(filter %.c,$(SOURCES_LIBS)))
OBJECTS_STATIC	+= $(patsubst %.cpp,$(BUILDDIR)/%.o,$(filter %.cpp,$(SOURCES))) $(patsubst %.cpp,$(BUILDDIR)/%-static.o,$(filter %.cpp,$(SOURCES_LIBS)))

LIBRARY	= $(PUPNP)/libpupnp.a \
		 $(CODECS)/$(HOST)/$(PLATFORM)/libcodecs.a \
		 
LIBRARY_STATIC 	= $(LIBRARY) $(OPENSSL)/libopenssl.a

all: directory $(EXECUTABLE) $(EXECUTABLE_STATIC)

$(EXECUTABLE): $(OBJECTS) $(DEPS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -o $@
	
$(EXECUTABLE_STATIC): $(OBJECTS_STATIC) $(DEPS)
	$(CC) $(OBJECTS_STATIC) $(LIBRARY_STATIC) $(LDFLAGS) -static -o $@	

directory:
	@mkdir -p $(BUILDDIR)
	@mkdir -p bin

$(BUILDDIR)/%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

$(BUILDDIR)/%-static.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DSSL_LIB_STATIC $(INCLUDE) $< -c -o $(BUILDDIR)/$*-static.o	
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(OBJECTS_STATIC) $(EXECUTABLE_STATIC) 

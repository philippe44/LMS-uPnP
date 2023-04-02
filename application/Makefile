ifeq ($(CC),cc)
CC=$(lastword $(subst /, ,$(shell readlink -f `which cc`)))
endif

ifeq ($(findstring gcc,$(CC)),gcc)
CFLAGS  += -Wno-deprecated-declarations -Wno-format-truncation -Wno-stringop-truncation 
LDFLAGS += -s -lstdc++
#LDFLAGS += -lstdc++
else
CFLAGS += -fno-temp-file
LDFLAGS += -lc++
endif

PLATFORM ?= $(firstword $(subst -, ,$(CC)))
HOST ?= $(word 2, $(subst -, ,$(CC)))

ifneq ($(HOST),macos)
ifneq ($(HOST),solaris)
LINKSTATIC = -static
else
LDFLAGS += -lssp	
endif
endif

CORE              = bin/squeeze2upnp-$(HOST)
BUILDDIR          = $(dir $(CORE))$(HOST)/$(PLATFORM)
EXECUTABLE        = $(CORE)-$(PLATFORM)
EXECUTABLE_STATIC = $(EXECUTABLE)-static

SRC	        = squeeze2upnp
SQUEEZELITE	= squeezelite
TOOLS		= crosstools/src
MDNS		= libmdns/targets
PUPNP 		= libpupnp/targets/$(HOST)/$(PLATFORM)
CODECS		= libcodecs/targets
OPENSSL		= libopenssl/targets/$(HOST)/$(PLATFORM)
#VALGRIND	= $(BASE)/valgrind

DEFINES 	= -DCODECS -DUSE_SSL -D_GNU_SOURCE -DUPNP_STATIC_LIB -DLINKALL -DRESAMPLE

CFLAGS  += -Wall -fPIC -ggdb -O2 $(DEFINES) -fdata-sections -ffunction-sections 
LDFLAGS += -lpthread -ldl -lm -L. 

vpath %.c $(TOOLS):$(SRC):$(SQUEEZELITE)
vpath %.cpp $(TOOLS):$(SRC):$(SQUEEZELITE)

INCLUDE = -I$(OPENSSL)/include \
		  -I$(TOOLS) \
		  -I$(SQUEEZELITE) \
		  -I$(SRC)/inc \
		  -I$(PUPNP)/include/upnp -I$(PUPNP)/include/ixml -I$(PUPNP)/include/addons \
		  -I$(CODECS)/include/flac -I$(CODECS)/include/mad \
		  -I$(CODECS)/include/ogg -I$(CODECS)/include/vorbis \
		  -I$(CODECS)/include/opus -I$(CODECS)/include/opusfile \
		  -I$(CODECS)/include/faad2 \
		  -I$(CODECS)/include/shine \
		  -I$(CODECS)/include/soxr -I$(CODECS)/include/addons \
		  -I$(MDNS)/include/mdnsvc -I$(MDNS)/include/mdnssd
		  		  		  
DEPS	= $(SRC)/inc/squeezedefs.h
				  
SOURCES = slimproto.c buffer.c output_http.c output.c main.c \
		  stream.c decode.c pcm.c resample.c process.c \
          alac.c flac.c mad.c vorbis.c opus.c faad.c \
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

$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) $(LIBRARY) $(CFLAGS) $(LDFLAGS) -o $@
ifeq ($(HOST),macos)
	rm -f $(CORE)
	lipo -create -output $(CORE) $$(ls $(CORE)* | grep -v '\-static')
endif

$(EXECUTABLE_STATIC): $(OBJECTS_STATIC) $(DEPS)
	$(CC) $(OBJECTS_STATIC) $(LIBRARY_STATIC) $(CFLAGS) $(LDFLAGS) $(LINKSTATIC) -o $@
ifeq ($(HOST),macos)
	rm -f $(CORE)-static
	lipo -create -output $(CORE)-static $(CORE)-*-static
endif
	

$(OBJECTS) $(OBJECTS_STATIC): $(DEPS)	

directory:
	@mkdir -p $(BUILDDIR)
	@mkdir -p bin

$(BUILDDIR)/%.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(INCLUDE) $< -c -o $@

$(BUILDDIR)/%-static.o : %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DSSL_STATIC_LIB $(INCLUDE) $< -c -o $(BUILDDIR)/$*-static.o	
	
clean:
	rm -f $(OBJECTS) $(EXECUTABLE) $(OBJECTS_STATIC) $(EXECUTABLE_STATIC) $(CORE) $(CORE)-static

# Cross compile support - create a Makefile which defines these three variables and then includes this Makefile...
#CC = arm-linux-gnueabi-gcc
CFLAGS  ?= -Wall -fPIC -ggdb -O2 $(OPTS) $(INCLUDE) $(DEFINES)
LDFLAGS ?= -lpthread -lm -lrt -L. 
# for LD debug -s
EXECUTABLE ?= squeeze2upnp-x86
EXECUTABLE_STATIC ?= squeeze2upnp-x86-static

# passing one or more of these in $(OPTS) enables optional feature inclusion
#OPT_DSD     = -DDSD
#OPT_FF      = -DFFMPEG
#OPT_LINKALL = -DLINKALL
#OPT_RESAMPLE= -DRESAMPLE
#OPT_VIS     = -DVISEXPORT

squeezetiny_dir = squeezetiny
squeezeupnp_dir = squeeze2upnp
build_dir	= ./bin

LIBRARY = libupnp.a libixml.a libthreadutil.a

DEFINES = -DNO_CODEC -D_FILE_OFFSET_BITS=64 
#-D_LARGE_FILE_SOURCE

INCLUDE = -I../../libupnp/1.6.19/threadutil/inc\
          -I../../libupnp/1.6.19/ixml/inc\
          -I../../libupnp/1.6.19/upnp/inc\
          -I../../libupnp/1.6.19/build/inc\
          -I$(squeezetiny_dir)\
          -I$(squeezeupnp_dir)/inc

SOURCES = \
	$(squeezetiny_dir)/slimproto.c $(squeezetiny_dir)/buffer.c \
        $(squeezetiny_dir)/stream.c $(squeezetiny_dir)/utils.c \
	$(squeezetiny_dir)/output_mr.c $(squeezetiny_dir)/decode.c \
        $(squeezetiny_dir)/main.c $(squeezetiny_dir)/util_common.c \
	$(squeezeupnp_dir)/avt_util.c $(squeezeupnp_dir)/mr_util.c \
      	$(squeezeupnp_dir)/util.c $(squeezeupnp_dir)/webserver.c \
        $(squeezeupnp_dir)/squeeze2upnp.c

#LINKALL          = -lFLAC -lmad -lvorbisfile -lfaad -lmpg123
#LINKALL_FF       = -lavcodec -lavformat -lavutil
#LINKALL_RESAMPLE = -lsoxr

DEPS             = $(squeezetiny_dir)/squeezelite.h $(squeezetiny_dir)/slimproto.h\
                   $(squeezetiny_dir)/squeezedefs.h $(squeezetiny_dir)/squeezeitf.h

UNAME            = $(shell uname -s)

# add optional sources
ifneq (,$(findstring $(OPT_DSD), $(CFLAGS)))
	SOURCES += $(SOURCES_DSD)
endif
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	SOURCES += $(SOURCES_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	SOURCES += $(SOURCES_RESAMPLE)
endif
ifneq (,$(findstring $(OPT_VIS), $(CFLAGS)))
	SOURCES += $(SOURCES_VIS)
endif

# add optional link options
ifneq (,$(findstring $(OPT_LINKALL), $(CFLAGS)))
	LDFLAGS += $(LINKALL)
ifneq (,$(findstring $(OPT_FF), $(CFLAGS)))
	LDFLAGS += $(LINKALL_FF)
endif
ifneq (,$(findstring $(OPT_RESAMPLE), $(CFLAGS)))
	LDFLAGS += $(LINKALL_RESAMPLE)
endif
else
# if not LINKALL and linux add LINK_LINUX
ifeq ($(UNAME), Linux)
	LDFLAGS += $(LINK_LINUX)
endif
endif

OBJECTS = $(SOURCES:.c=.o)

all: $(EXECUTABLE) $(EXECUTABLE_STATIC)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -o $(build_dir)/$@

$(EXECUTABLE_STATIC): $(OBJECTS)
	$(CC) $(OBJECTS) $(LIBRARY) $(LDFLAGS) -static -o $(build_dir)/$@

$(OBJECTS): $(DEPS)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) $< -c -o $@


clean:
	rm -f $(OBJECTS) $(EXECUTABLE)


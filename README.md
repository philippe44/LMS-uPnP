This application is a bridge between Logitech Media Server (LMS) /
SqueezeBox Server(SBS) and UPnP devices. It allows any UPnP device 
to be used as a normal SqueezeBox device.

Packaged versions can found [here](https://sourceforge.net/projects/lms-plugins-philippe44/ and here:https://sourceforge.net/projects/lms-to-upnp)

See support thread here: http://forums.slimdevices.com/showthread.php?103728-Announce-UPnPBridge-integrate-UPnP-DLNA-players-with-LMS-(squeeze2upnp)&p=820082&viewfull=1#post820082

# MINIMUM TO READ FOR ADVANCED USE
If you do not want to RTFM, at least do the following:
- put the application on a LOCAL disk where you have read/write access
- launch it with "-i config.xml" on the command line, and wait 30s till it exits
- launch the application without parameters (type exit to stop it)
- if it does not work try again with firewall disabled 
- ALWAYS start with a very low volume in LMS (some players might play very loud)
- pay at least attention to the following parameters in the config.xml file 

	* <stream_length>    : if nothing is played
	* <accept_nexturi>   : if playback is stuck in playlist
	* <seek_after_pause> : if resume after pause does not restart

Really, have a look at the user guide in /doc

# WHAT VERSION TO CHOOSE
Download UPnPBridge.zip and in /Bin, there are binaries for Windows, Linux 
(x86, x86_64, arm, ppc and sparc) and OSX. 

NB: Main tests are on Windows 7 and Linux x86 versions. OSX and Linux ARM 
are "as is"

# ADVANCED
REBUILDING IS NOT NEEDED, but if you want to rebuild, here is what should be 
done. This can be built for Linux, OSX or Windows. My main Windows environment 
is C++ builder, so I've not made a proper makefile. The Linux/OSX makefile is
very hardcoded.

DEPENDENCIES
 - pthread for Windows: https://www.sourceware.org/pthreads-win32
 - libupnp: https://sourceforge.net/projects/pupnp
 - ALAC codec: https://github.com/philippe44/alac
 - faad2: http://www.audiocoding.com/
 - libmad: https://www.underbit.com/products/mad/
 - libflac: https://xiph.org/flac/
 - libsoxr: https://sourceforge.net/p/soxr/wiki/Home/
 - libogg, libopus & libvorbis: https://xiph.org/vorbis/
 - shine: https://github.com/philippe44/shine

LIBUPNP
 - Under Win32, libupnp is build with the following defines
        WIN32
        UPNP_STATIC_LIB
        UPNP_USE_BCBPP (for C++ builder)
 - TCP blocking *must* be disabled otherwise when a upnp player disappears
"non gracefully", it will block other communication for a long delay, creating
a "too many job" log in libupnp and all sort of nasty side effects. When 
launching ./configure, use --disable-blocking_tcp_connections to make sure that
autoconfig.h in the root directory (and in build/inc) have the #define
UPNP_ENABLE_BLOCKING_TCP_CONNECTIONS *not* defined or commented
- It is recommended to make the following changes to config.h (find the 
existing corresponding lines)
	#define MAX_JOBS_TOTAL 200 
	#define GENA_NOTIFICATION_SENDING_TIMEOUT 5
	#define GENA_NOTIFICATION_ANSWERING_TIMEOUT 5

Main application for Windows XP and above
 - For compilation, use following defines
        WIN32
        UPNP_STATIC_LIB
        UPNP_USE_BCBPP (for C++ builder)
        NO_CODEC
 - Link with the generated libupnp static lib and ptrhead import lib
 - For runtime
        On C++ builder, requires cc32160mt.dll
        pthread.dll for all compilers

Main application for Linux and OSX
 - For compilation, use following defines
        NO_CODEC
        _FILE_OFFSET_BITS=64
 - Put the 3 libraries of libupnp (found in ./libs) in the Makefile directory







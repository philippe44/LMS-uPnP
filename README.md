# LMS to UPnP bridge
Allows UPnP players to be used by Logitech Media Server, as a normal Logitech SqueezeBox. 

This project is now part of LMS 3rd parties repositories. Support thread is [here](https://forums.slimdevices.com/showthread.php?103728-Announce-UPnPBridge-integrate-UPnP-DLNA-players-with-LMS-(squeeze2upnp))

Please see [here](https://github.com/philippe44/cross-compiling/blob/master/README.md#organizing-submodules--packages) to know how to rebuild my apps in general

Otherwise, you can just get the source code and pre-built binaries:
```
cd ~
git clone http://github.com/philippe44/lms-upnp
cd ~/lms-upnp
git submodule update --init
```
and build doing:
```
cd ~/lms-upnp/application
make
```

Binary releases are [here](https://sourceforge.net/projects/lms-plugins-philippe44/files/) as well

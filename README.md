# R8500-V1.0.2.116_1.0.90

Source File forked from Netgear, version 1.0.2.116

https://www.downloads.netgear.com/files/GPL/R8500-V1.0.2.116_1.0.90_src.tar.zip

Fixed compile error on/for Ubuntu 16.04 x64.

Need to add som packages first:
```
sudo dpkg-reconfigure dash
sudo apt install libogg-dev
sudo ln -s /usr/bin/arch /bin/arch
sudo apt install texinfo
sudo apt install intltool
```

Then follow the build instructions.

Note: Generated package not tested on real router.

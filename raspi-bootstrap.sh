#!/bin/sh
sudo apt-get install libusb-dev libdbus-1-dev libglib2.0-dev automake libudev-dev libical-dev libreadline-dev libtool
./bootstrap
./configure --disable-systemd
make

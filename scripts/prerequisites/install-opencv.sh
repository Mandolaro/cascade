#!/bin/bash
export INSTALL_PREFIX=$HOME/opt-dev/
# install python opencv
sudo apt-get -y install python3-opencv
# Download and unpack sources
wget -O opencv.zip https://github.com/opencv/opencv/archive/master.zip
unzip opencv.zip
# Create build directory
mkdir -p build && cd build
# Configure
cmake -D CMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} ../opencv-master
# Build
make -j `nproc`
make install

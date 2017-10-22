#!/bin/bash

g++ -O3 -Wall -Wextra -std=c++11 -g monitor_image.cpp -o monitor_image -lX11 -lfreeimage

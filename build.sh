#!/bin/bash

g++ -g -Wall -fPIC -DPIC -c main.cpp -I/usr/include/libdrm
g++ -g -shared main.o -o overload.so

#!/usr/bin/env bash

mkdir -p ./build/
sudo rm -rf ./build/*
cd ./build/
cmake ..
sudo make install

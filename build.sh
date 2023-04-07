#!/bin/bash

source ./env
source utils/utils.sh

info "Checking or building deps..."
./utils/build_deps.sh

cmake . -B build/ -DWASM=1 -DBoost_DEBUG=1 -DUSE_LZMA=1 -DUSE_EMBOOST=1 && info "Done Cmake" || die "cmake failed"
make -C build/ -j$MAKE_J && info "Done Cmake" || die "cmake failed"

test_html && info "Built successfully, run emrun build/index.html to try Innoextract!"

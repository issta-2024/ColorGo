# 使用一个基础镜像
# FROM zjuchenyuan/base
FROM ubuntu:20.04

# clone unibench
RUN apt-get update && apt-get install -y git && apt-get install -y unzip

RUN apt-get install -y llvm-10 clang-10

RUN apt-get install -y make

# RUN apt-get install -y z3

RUN git clone https://github.com/UNIFUZZ/unibench &&\
    cd unibench && \
    mkdir mp3gain-1.5.2 && cd mp3gain-1.5.2 && mv ../mp3gain-1.5.2.zip ./ && unzip -q mp3gain-1.5.2.zip && rm mp3gain-1.5.2.zip && cd .. &&\
    ls *.zip|xargs -i unzip -q '{}' &&\
    ls *.tar.gz|xargs -i tar xf '{}' &&\
    rm -r .git/ *.tar.gz *.zip &&\
    mv SQLite-8a8ffc86 SQLite-3.8.9 && mv binutils_5279478 binutils-5279478 && mv libtiff-Release-v3-9-7 libtiff-3.9.7 &&\
    ls -alh

# 将脚本文件复制到容器内
RUN mkdir -p ~/verification/symcc
RUN mkdir /benchmark
ADD ./program /benchmark/program
ADD ./bash /benchmark/bash
ADD ./seeds /benchmark/seeds
ADD ./symcc ~/verification/symcc

ADD ./z3 ~/verification/z3

# RUN find / -name "clang-10" 2>/dev/null

# RUN find / -name "libz3.so" 2>/dev/null
ENV PATH="/usr/lib/llvm-10/bin:$PATH"
# ENV LD_LIBRARY_PATH="~/verification/z3/build:$LD_LIBRARY_PATH"

# RUN clang -v



ENV DSYMCC=~/verification/symcc/build_new \
    CC=~/verification/symcc/build_new/symcc \
    CXX=~/verification/symcc/build_new/sym++

# 设置target_dir变量
ENV TMP_DIR=/benchmark/program/mujs/temp/target1

# RUN ls -l /usr/lib/llvm-10/lib

# RUN ls -l /usr/lib/llvm-10/bin

# RUN ls -l /usr/lib/x86_64-linux-gnu

# 编译
RUN cd /unibench/mujs-1.0.2 &&\
    sed -i '1024s/$/fopen("target","w");exit(0);/' jsrun.c &&\
    build=debug make -j &&\
    cp -f build/debug/mujs /benchmark/program/mujs/target1/mujs &&\
    make clean &&\
    sed -i '1024s/fopen("target","w");exit(0);$//' jsrun.c

RUN cd /benchmark/program/mujs/target1 &&\
    /benchmark/bash/aaa_execution_deep_1.sh -i /benchmark/seeds/general_evaluation/mujs ./mujs -f @@


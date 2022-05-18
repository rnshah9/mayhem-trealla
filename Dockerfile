FROM --platform=linux/amd64 ubuntu:20.04

RUN apt-get update
RUN DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential libreadline-dev xxd libssl-dev

ADD . /repo
WORKDIR /repo
RUN make -j8

FROM registry.cn-hangzhou.aliyuncs.com/test_tc/llvm_hw:0.2

RUN apt update -y && \
    apt install -y gdb

CMD ["/bin/bash"]
CXX ?= g++
NVCC ?= /opt/cuda/bin/nvcc

OBS_CFLAGS ?= -I/usr/include/obs -DHAVE_OBSCONFIG_H
OBS_LIBS   ?= -lobs
CUDA_PATH  ?= /opt/cuda

CXXFLAGS := -std=c++17 -O3 -fPIC -Wall -Wextra $(OBS_CFLAGS) -Isrc -I$(CUDA_PATH)/include
NVCCFLAGS := -O3 -Xcompiler -fPIC -std=c++17 -arch=sm_89 -Isrc -I$(CUDA_PATH)/include
LDFLAGS := -shared $(OBS_LIBS) -L$(CUDA_PATH)/targets/x86_64-linux/lib -L$(CUDA_PATH)/lib64 -lcudart -ldl

TARGET := linrtx-obs.so

OBJS := \
    src/plugin-main.o \
    src/nvvfx-loader.o \
    src/cuda-gl-interop.o \
    src/filter-vsr.o \
    src/filter-truehdr.o \
    src/filter-framegen.o \
    src/kernels/format_convert.o

USER_PLUGIN_DIR := $(HOME)/.config/obs-studio/plugins/linrtx-obs

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -shared -o $@ $^ $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

src/kernels/%.o: src/kernels/%.cu
	$(NVCC) $(NVCCFLAGS) -c $< -o $@

install-user: $(TARGET)
	mkdir -p $(USER_PLUGIN_DIR)/bin/64bit
	mkdir -p $(USER_PLUGIN_DIR)/data
	cp -f $(TARGET) $(USER_PLUGIN_DIR)/bin/64bit/
	cp -rf data/* $(USER_PLUGIN_DIR)/data/
	@echo "Installed LinRTX-OBS plugin to $(USER_PLUGIN_DIR)"

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install-user clean

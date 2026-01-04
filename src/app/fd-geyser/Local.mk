# fd-geyser: Real-time gRPC Geyser service for Firedancer
#
# This service provides transaction-level real-time streaming of account updates
# with entry boundary markers, as opposed to waiting for slot completion.
#
# Dependencies: grpc, protobuf, abseil

# Location of grpc installation (adjust as needed)
PKG ?= /home/ubuntu/solana_fd/pkg

CPPFLAGS += -I$(PKG)/include

# C++ flags for C/C++ compatibility when including C headers
# -fpermissive: Allow C-style implicit conversions (void* to typed pointers)
# -Wno-error: Don't treat warnings as errors for C++ (firedancer C headers aren't C++ safe)
# -Wno-vla-cxx-extension: Allow C99 VLA syntax
CXXFLAGS += -fpermissive -Wno-write-strings -Wno-error -Wno-vla-cxx-extension

# gRPC and Protobuf libraries
GRPC_LIBS += -Wl,--start-group $(wildcard $(PKG)/lib/libgrpc*.a) -Wl,--end-group
GRPC_LIBS += -Wl,--start-group $(wildcard $(PKG)/lib/libupb_*.a) -Wl,--end-group
GRPC_LIBS += -Wl,--start-group $(wildcard $(PKG)/lib/libprotobuf*.a) -Wl,--end-group
GRPC_LIBS += -Wl,--start-group $(wildcard $(PKG)/lib/libabsl_*.a) -Wl,--end-group
GRPC_LIBS += $(PKG)/lib/libgpr.a
GRPC_LIBS += $(PKG)/lib/libaddress_sorting.a
GRPC_LIBS += $(PKG)/lib/libcares.a
GRPC_LIBS += $(PKG)/lib/libre2.a
GRPC_LIBS += $(PKG)/lib/libssl.a
GRPC_LIBS += $(PKG)/lib/libcrypto.a
GRPC_LIBS += $(PKG)/lib/libz.a
GRPC_LIBS += $(PKG)/lib/libutf8_range_lib.a
GRPC_LIBS += $(PKG)/lib/libutf8_validity.a
GRPC_LIBS += -pthread -ldl -lsystemd

# Build the fd_geyser binary
# Sources: main, service, filter, poller, and generated protobuf files
$(call make-bin,fd_geyser,fd_geyser_main fd_geyser_service fd_geyser_filter fd_geyser_poller geyser.grpc.pb geyser.pb solana-storage.pb,fd_discof fd_disco fd_flamenco fd_reedsol fd_funk fd_tango fd_choreo fd_waltz fd_ballet fd_util,$(SECP256K1_LIBS) $(GRPC_LIBS))

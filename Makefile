# QNX UKI Filesystem Resource Manager Makefile
# Supports: x86_64 and aarch64 architectures

# Project name
TARGET = ukifs

# Location of the ukiboot parent project
UKIBOOT ?= ..

# Location of QNX SDP (relative to current directory)
QNX_SDP ?= qnx710
QNX_HOST ?= $(QNX_SDP)/host/linux/x86_64
QNX_TARGET ?= $(QNX_SDP)/target/qnx7

# Check if QNX SDP exists
ifeq ($(wildcard $(QNX_TARGET)),)
$(error QNX SDP not found at $(QNX_TARGET))
endif

# Target architecture (can be overridden: make ARCH=aarch64)
ARCH ?= x86_64

# Supported architectures (64-bit only)
SUPPORTED_ARCHS = x86_64 aarch64

# Location of compat headers
COMPAT=compat

# Validate architecture
ifeq ($(filter $(ARCH),$(SUPPORTED_ARCHS)),)
$(error Unsupported ARCH=$(ARCH). Supported: $(SUPPORTED_ARCHS))
endif

# QNX toolchain prefix
QNX_BIN = $(QNX_HOST)/usr/bin

# Architecture-specific settings
ifeq ($(ARCH),x86_64)
    CFLAGS_ARCH = -D__X86_64__
    CC = $(QNX_BIN)/x86_64-pc-nto-qnx7.1.0-gcc
    LD = $(QNX_BIN)/x86_64-pc-nto-qnx7.1.0-gcc
    STRIP = $(QNX_BIN)/x86_64-pc-nto-qnx7.1.0-strip
    TARGET_SUFFIX =
endif

ifeq ($(ARCH),aarch64)
    CFLAGS_ARCH = -D__AARCH64__ -D__ARM__
    CC = $(QNX_BIN)/aarch64-unknown-nto-qnx7.1.0-gcc
    LD = $(QNX_BIN)/aarch64-unknown-nto-qnx7.1.0-gcc
    STRIP = $(QNX_BIN)/aarch64-unknown-nto-qnx7.1.0-strip
    TARGET_SUFFIX = .aarch64
endif

CFLAGS_WARN = -Wall -Wextra -Wpedantic -Wshadow #-Wconversion

# Common compiler flags
CFLAGS_COMMON = -O2 -g \
		-D__QNXNTO__ -D_QNX_SOURCE -D__LITTLEENDIAN__ \
		-I. -I$(UKIBOOT) -I$(COMPAT) \
		-I$(QNX_TARGET)/usr/include \

# Final compiler flags (common + architecture-specific)
CFLAGS = $(CFLAGS_COMMON) $(CFLAGS_ARCH) $(CFLAGS_WARN)

# Linker flags
LDFLAGS = -L$(QNX_TARGET)/$(ARCH)/lib \
	  -L$(QNX_TARGET)/$(ARCH)/usr/lib

# Libraries to link
LIBS = -lssl -lcrypto

# Source files
SRCS = ukifs_core.c \
       ukifs_parse.c \
       ukifs_io.c \
       ukifs_dir.c \
       ukifs_utils.c \
       ukifs_size.c \
       $(COMPAT)/ukifs_ssl.c \
       $(UKIBOOT)/pe_parser_posix.c \
       $(UKIBOOT)/utils.c

# Object files (architecture-specific)
OBJS = $(SRCS:.c=.$(ARCH).o)

# Header files
HDRS = ukifs_core.h \
       ukifs_dir.h \
       ukifs_io.h \
       ukifs_parse.h \
       ukifs_pe.h \
       ukifs_size.h \
       ukifs_utils.h \
       $(COMPAT)/config.h \
       $(COMPAT)/sys/endian.h \
       $(UKIBOOT)/pe_parser_posix.h \
       $(UKIBOOT)/utils.h \

# Final target name (with architecture suffix for cross-builds)
TARGET_BIN = $(TARGET)$(TARGET_SUFFIX)

# Default target - build both architectures
.PHONY: all
all: all-archs

# Build banner
.PHONY: banner
banner:
	@echo "========================================"
	@echo "Building QNX UKI Filesystem Driver"
	@echo "Architecture: $(ARCH)"
	@echo "Compiler: $(CC)"
	@echo "Target: $(TARGET_BIN)"
	@echo "========================================"


# Link
$(TARGET_BIN): $(OBJS)
	@echo "Linking $@ for $(ARCH)..."
	QNX_HOST=$(QNX_HOST) QNX_TARGET=$(QNX_TARGET) PATH=$(QNX_BIN):$$PATH \
	    $(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo "Build complete: $@"

# Compile
%.$(ARCH).o: %.c $(HDRS)
	@echo "Compiling $< for $(ARCH)..."
	QNX_HOST=$(QNX_HOST) QNX_TARGET=$(QNX_TARGET) PATH=$(QNX_BIN):$$PATH \
	    $(CC) $(CFLAGS) -c $< -o $@

# Clean (remove all architectures)
.PHONY: clean
clean:
	@echo "Cleaning..."
	@$(MAKE) clean-arch ARCH=x86_64
	@$(MAKE) clean-arch ARCH=aarch64
	@$(MAKE) clean-tests
	@echo "Clean complete"

# Clean specific architecture
.PHONY: clean-arch
clean-arch:
	@echo "Cleaning $(ARCH) artifacts..."
	rm -f $(OBJS) $(TARGET_BIN)
	@echo "Clean complete for $(ARCH)"

# Build all supported architectures
.PHONY: all-archs
all-archs:
	@echo "Building for all architectures..."
	@$(MAKE) clean-arch single-arch ARCH=x86_64
	@$(MAKE) clean-arch single-arch ARCH=aarch64
	@echo "All architectures built successfully!"
	@ls -lh ukifs ukifs.aarch64

# Build single architecture (internal target)
.PHONY: single-arch
single-arch: banner $(TARGET_BIN)

# Cross-compilation helper targets
.PHONY: arm64 armv8 aarch64
arm64 aarch64 armv8:
	@$(MAKE) single-arch ARCH=aarch64

.PHONY: x64 x86 x86_64
x64 x86 x86_64:
	@$(MAKE) single-arch ARCH=x86_64

.PHONY: help
help:
	@echo "QNX UKI Filesystem"
	@echo ""
	@echo "Supported Architectures (64-bit only):"
	@echo "	 x86_64	      - 64-bit Intel/AMD (default)"
	@echo "	 aarch64      - 64-bit ARM (ARMv8)"
	@echo ""
	@echo "Build Targets:"
	@echo "	 all	      - Build for current ARCH (default: x86_64)"
	@echo "	 all-archs    - Build for all supported architectures"
	@echo "	 clean	      - Remove all build artifacts"
	@echo "	 clean-arch   - Remove artifacts for current ARCH only"
	@echo ""
	@echo "Architecture Shortcuts:"
	@echo "	 make arm64    # Build for ARMv8 (aarch64)"
	@echo "	 make x64      # Build for x86_64"
	@echo ""
	@echo "Usage Examples:"
	@echo "	 make	       # Build for all architectures"
	@echo "	 make tests    # Build all test programs"
	@echo "	 make clean    # Clean all artifacts"

# Host test programs
.PHONY: tests
tests: test-verify test-size
	@echo "All test programs built"

clean-tests:
	rm -f test_verify test_size

.PHONY: test-verify
test-verify: test_verify
	@echo "Test program built: test_verify"
	@echo "Usage: ./test_verify <uki-file> <cert.pem>"

test_verify: test_verify.c \
	     ukifs_size.c ukifs_size.h \
	     $(UKIBOOT)/pe_parser_posix.c \
	     $(UKIBOOT)/pe_parser_posix.h \
	     $(UKIBOOT)/utils.h \
	     $(UKIBOOT)/utils.c $(COMPAT)/config.h
	@echo "Building signature verification test..."
	gcc -o test_verify \
	    test_verify.c ukifs_size.c \
	    $(UKIBOOT)/pe_parser_posix.c \
	    $(UKIBOOT)/utils.c \
	    -Wall -Wextra -O2 -g \
	     -I. -I$(UKIBOOT) -I$(COMPAT) \
	    $(LIBS)
	@echo "Build complete: test_verify"

.PHONY: test-size
test-size: test_size
	@echo "Test program built: test_size"
	@echo "Usage: ./test_size <uki-file>"

test_size: test_size.c \
	   ukifs_size.c ukifs_size.h \
	   ukifs_utils.c ukifs_size.h \
	   $(UKIBOOT)/utils.c $(UKIBOOT)/utils.h
	@echo "Building UKI size analysis test..."
	gcc -o test_size \
	    test_size.c ukifs_size.c \
	    $(UKIBOOT)/utils.c \
	    -Wall -Wextra -O2 -g \
	    -I. -I$(UKIBOOT) -I$(COMPAT)
	@echo "Build complete: test_size"

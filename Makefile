# ---------------------------------------------------------------------------
# Makefile — nvme.device for AmigaOS 4.1 Final Edition
#
# Two build flavours share this Makefile:
#
#   make          — release build: build/nvme.device, build/test_nvme
#                   (DPRINTF compiled out; always-on DLOG/banner still fire)
#   make debug    — debug build:   build/nvme.device.debug,
#                                  build/test_nvme.debug
#                   (DPRINTF active, banner notes "DEBUG build")
#   make all      — both of the above in one invocation
#
# After building (normally via the walkero/amigagccondocker:os4-gcc11 image
# under WSL2), copy the chosen flavour to Kickstart staging from the host:
#
#   make deploy        — copy release driver + test to $(DEPLOY_DIR)
#   make deploy-debug  — copy debug driver + test to $(DEPLOY_DIR)
#                        (driver always lands as nvme.device regardless of
#                        flavour, so kickstart.zip injection is unchanged)
#
#   DEPLOY_DIR defaults to /mnt/s/temp.  Override with:
#       make deploy DEPLOY_DIR=/some/other/path
#
# BUILD_DATE and BUILD_TIME are re-stamped on every compile so the runtime
# banner identifies the exact build.
# ---------------------------------------------------------------------------

CC = ppc-amigaos-gcc

# Dynamic build stamp
BUILD_DATE := $(shell date +"%d.%m.%Y")
BUILD_TIME := $(shell date +"%H:%M:%S")

# SMART / Health Information periodic refresh.  On by default;
# override with `make NO_SMART=1` to drop the SMART admin-command path.
SMART_FLAG := -DENABLE_SMART
ifeq ($(NO_SMART),1)
    SMART_FLAG :=
endif

COMMON_CFLAGS = -O2 -Wall -I./include \
                -fno-tree-loop-distribute-patterns \
                -DBUILD_DATE='"$(BUILD_DATE)"' \
                -DBUILD_TIME='"$(BUILD_TIME)"' \
                $(SMART_FLAG)

LDFLAGS = -nostartfiles

# Paths ---------------------------------------------------------------------
BUILD_DIR  = build
REL_DIR    = $(BUILD_DIR)/release
DBG_DIR    = $(BUILD_DIR)/debug
REL_TARGET = $(BUILD_DIR)/nvme.device
DBG_TARGET = $(BUILD_DIR)/nvme.device.debug
REL_TEST   = $(BUILD_DIR)/test_nvme
DBG_TEST   = $(BUILD_DIR)/test_nvme.debug
REL_STATS  = $(BUILD_DIR)/nvme_stats
DBG_STATS  = $(BUILD_DIR)/nvme_stats.debug

DEPLOY_DIR ?= /mnt/s/temp

# AmiUpdate integration — dist and dist-lha targets use these.
AMIUPDATE_DIR    ?= ../AmiUpdateIntegration
AMIUPDATE_CONFIG ?= amiupdate.yml
DIST_DIR          = $(BUILD_DIR)/dist
DIST_NAME         = nvme
DIST_STAGE        = $(DIST_DIR)/$(DIST_NAME)
DIST_LHA          = $(BUILD_DIR)/$(DIST_NAME).lha

# Docker image used for the LHA packer (lha is not always on the host).
DOCKER_IMAGE     ?= walkero/amigagccondocker:os4-gcc11

# Sources -------------------------------------------------------------------
SRC = src/device.c src/Init.c src/Open.c src/Close.c src/Expunge.c src/BeginIO.c \
      src/unit_discovery.c src/unit_task.c \
      src/nvme/nvme_init.c src/nvme/nvme_io.c src/nvme/nvme_admin.c src/nvme/nvme_irq.c \
      src/pci/pci_discovery.c src/pci/platform_detect.c \
      src/scsi_cmds/scsi_ata_passthrough.c src/scsi_cmds/scsi_log_sense.c \
      src/scsi_cmds/scsi_unmap.c src/scsi_cmds/scsi_mode.c \
      src/nvme_mmu.c src/nvme_status.c src/nvme_leak.c src/nvme_stats.c \
      src/compat.c

REL_OBJ = $(patsubst src/%.c, $(REL_DIR)/%.o, $(SRC))
DBG_OBJ = $(patsubst src/%.c, $(DBG_DIR)/%.o, $(SRC))

.PHONY: all release debug clean deploy deploy-debug dist dist-lha

# Default target builds both flavours so you always get a matched pair.
all: release debug

release: $(REL_TARGET) $(REL_TEST) $(REL_STATS)

debug: $(DBG_TARGET) $(DBG_TEST) $(DBG_STATS)

# Release link --------------------------------------------------------------
$(REL_TARGET): $(REL_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

$(REL_TEST): tests/test_nvme.c $(REL_TARGET)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $< -o $@ -lauto

$(REL_STATS): tests/nvme_stats.c $(REL_TARGET)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) $< -o $@ -lauto

# Debug link ----------------------------------------------------------------
$(DBG_TARGET): $(DBG_OBJ)
	$(CC) $^ -o $@ $(LDFLAGS)

$(DBG_TEST): tests/test_nvme.c $(DBG_TARGET)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -DDEBUG $< -o $@ -lauto

$(DBG_STATS): tests/nvme_stats.c $(DBG_TARGET)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(COMMON_CFLAGS) -DDEBUG $< -o $@ -lauto

# Per-variant compile rules -------------------------------------------------
$(REL_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

$(DBG_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -DDEBUG -c $< -o $@

# Deploy --------------------------------------------------------------------
# Note: deploy targets are plain `cp` and are intended to be run from the
# host shell (outside Docker) — they do not require the PPC toolchain.
deploy: $(REL_TARGET) $(REL_TEST) $(REL_STATS)
	@test -d $(DEPLOY_DIR) || { echo "DEPLOY_DIR $(DEPLOY_DIR) does not exist"; exit 1; }
	cp -f $(REL_TARGET) $(DEPLOY_DIR)/nvme.device
	cp -f $(REL_TEST)   $(DEPLOY_DIR)/test_nvme
	cp -f $(REL_STATS)  $(DEPLOY_DIR)/nvme_stats
	@echo "Deployed release build to $(DEPLOY_DIR)/"

deploy-debug: $(DBG_TARGET) $(DBG_TEST) $(DBG_STATS)
	@test -d $(DEPLOY_DIR) || { echo "DEPLOY_DIR $(DEPLOY_DIR) does not exist"; exit 1; }
	cp -f $(DBG_TARGET) $(DEPLOY_DIR)/nvme.device
	cp -f $(DBG_TEST)   $(DEPLOY_DIR)/test_nvme
	cp -f $(DBG_STATS)  $(DEPLOY_DIR)/nvme_stats
	@echo "Deployed DEBUG build to $(DEPLOY_DIR)/ (lands as nvme.device)"

# Distribution ---------------------------------------------------------------
#
# `make dist` stages an AmiUpdate-ready drawer under $(DIST_DIR)/nvme/ —
# both driver flavours, the test program, the stats monitor, the plain-
# text readme, a diskboot.config sample, and a generated AutoInstall
# script (via $(AMIUPDATE_DIR)/generate_autoinstall.py).
#
# `make dist-lha` wraps the staging directory + a copy of the readme at
# the archive root into an LHA — runs lha inside the toolchain Docker
# image so it works from any host that can execute docker.
$(DIST_STAGE)/AutoInstall: $(AMIUPDATE_CONFIG) $(AMIUPDATE_DIR)/generate_autoinstall.py
	@mkdir -p $(DIST_STAGE)
	python3 $(AMIUPDATE_DIR)/generate_autoinstall.py $(AMIUPDATE_CONFIG) $@

dist: release debug $(DIST_STAGE)/AutoInstall nvme.readme diskboot.config.sample
	@echo "=== Staging distribution tree in $(DIST_DIR) ==="
	@mkdir -p $(DIST_STAGE)
	cp -f $(REL_TARGET)             $(DIST_STAGE)/nvme.device
	cp -f $(DBG_TARGET)             $(DIST_STAGE)/nvme.device.debug
	cp -f $(REL_TEST)               $(DIST_STAGE)/test_nvme
	cp -f $(REL_STATS)              $(DIST_STAGE)/nvme_stats
	cp -f nvme.readme               $(DIST_STAGE)/nvme.readme
	cp -f diskboot.config.sample    $(DIST_STAGE)/diskboot.config.sample
	cp -f nvme.readme               $(DIST_DIR)/nvme.readme
	@echo ""
	@echo "Staged contents:"
	@find $(DIST_STAGE) -type f | sort
	@echo ""
	@echo "Run 'make dist-lha' to pack into $(DIST_LHA)."

dist-lha: dist
	@echo "=== Packing $(DIST_LHA) ==="
	@rm -f $(DIST_LHA)
	@# Run lha inside Docker so a host without lha installed still works.
	docker run --rm -v "$(CURDIR):/work" -w /work/$(DIST_DIR) $(DOCKER_IMAGE) \
	    sh -c 'lha ao5q /work/$(DIST_LHA) $(DIST_NAME) nvme.readme'
	@ls -la $(DIST_LHA)

clean:
	rm -rf $(BUILD_DIR)

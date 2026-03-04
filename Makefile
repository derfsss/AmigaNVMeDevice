CC = ppc-amigaos-gcc
CFLAGS = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns
LDFLAGS = -nostartfiles

BUILD_DIR = build
TARGET = $(BUILD_DIR)/nvme.device

SRC = src/device.c src/Init.c src/Open.c src/Close.c src/Expunge.c src/BeginIO.c \
      src/unit_discovery.c src/unit_task.c \
      src/exec_cmds/cmd_read.c src/exec_cmds/cmd_write.c \
      src/exec_cmds/cmd_update.c src/exec_cmds/cmd_stubs.c \
      src/nvme/nvme_init.c src/nvme/nvme_io.c src/nvme/nvme_admin.c \
      src/pci/pci_discovery.c

OBJ = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))

all: $(BUILD_DIR) $(TARGET) $(BUILD_DIR)/test_nvme

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(BUILD_DIR)/test_nvme: tests/test_nvme.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@ -lauto

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

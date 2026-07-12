# RedisJSON-c — C implementation of the ReJSON module
# 
# Build: make
# Output: rejson-c.so in project root (copy to tests/modules/ for TCL tests)

STD   = -std=gnu11
WARN  = -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas
OPT   = -Os -g -fPIC

REDIS_SRC_DIR = ../../src

CFLAGS = $(STD) $(WARN) $(OPT) \
    -I. \
    -I$(REDIS_SRC_DIR) \
    -DUSE_REDIS_MODULE_API

LDFLAGS = -shared -fPIC $(OPT)

SRCDIR = src
OBJDIR = obj

SRCS = $(wildcard $(SRCDIR)/*.c)
OBJS = $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))
TARGET = rejson-c.so

.PHONY: all clean install

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Copy to location expected by TCL tests
install: $(TARGET)
	cp $(TARGET) ../../tests/modules/rejson-c.so

clean:
	rm -rf $(OBJDIR) $(TARGET)

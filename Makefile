CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -D_POSIX_C_SOURCE=200809L
SRCDIR  := src
OBJDIR  := build
BIN     := cpu_thermal_guard

SRCS := $(wildcard $(SRCDIR)/*.c)
OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all clean run install uninstall

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: all
	./$(BIN) -v --dry-run

# 安装二进制到 /usr/local/sbin 并注册 systemd 服务 (需 root)
PREFIX  ?= /usr/local
SBINDIR := $(PREFIX)/sbin
UNITDIR := /etc/systemd/system

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -m 0644 cpu-thermal-guard.service $(DESTDIR)$(UNITDIR)/cpu-thermal-guard.service
	@echo "已安装。启用开机自启: systemctl daemon-reload && systemctl enable --now cpu-thermal-guard"

uninstall:
	-systemctl disable --now cpu-thermal-guard 2>/dev/null || true
	rm -f $(DESTDIR)$(SBINDIR)/$(BIN)
	rm -f $(DESTDIR)$(UNITDIR)/cpu-thermal-guard.service
	-systemctl daemon-reload 2>/dev/null || true

clean:
	rm -rf $(OBJDIR) $(BIN)

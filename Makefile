CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2
CFLAGS  += -D_POSIX_C_SOURCE=200809L
SRCDIR  := src
OBJDIR  := build
BIN     := cpu_thermal_guard
CTL     := ctlguard

# 守护进程对象 (排除 ctl.c)
DAEMON_SRCS := $(filter-out $(SRCDIR)/ctl.c,$(wildcard $(SRCDIR)/*.c))
DAEMON_OBJS := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(DAEMON_SRCS))
CTL_OBJS    := $(OBJDIR)/ctl.o

.PHONY: all clean run test install uninstall

all: $(BIN) $(CTL)

test: tests/test_scheduler
	./tests/test_scheduler

$(BIN): $(DAEMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(CTL): $(CTL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

run: $(BIN)
	./$(BIN) -v --dry-run

# 安装二进制到 /usr/local/sbin 并注册 systemd 服务 (需 root)
PREFIX  ?= /usr/local
SBINDIR := $(PREFIX)/sbin
BINDIR  := $(PREFIX)/bin
UNITDIR := /etc/systemd/system

install: all
	install -d $(DESTDIR)$(SBINDIR)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(SBINDIR)/$(BIN)
	install -m 0755 $(CTL) $(DESTDIR)$(BINDIR)/$(CTL)
	install -m 0644 cpu-thermal-guard.service $(DESTDIR)$(UNITDIR)/cpu-thermal-guard.service
	@echo "已安装。启用开机自启: systemctl daemon-reload && systemctl enable --now cpu-thermal-guard"

uninstall:
	-systemctl disable --now cpu-thermal-guard 2>/dev/null || true
	rm -f $(DESTDIR)$(SBINDIR)/$(BIN)
	rm -f $(DESTDIR)$(BINDIR)/$(CTL)
	rm -f $(DESTDIR)$(UNITDIR)/cpu-thermal-guard.service
	-systemctl daemon-reload 2>/dev/null || true

clean:
	rm -rf $(OBJDIR) $(BIN) $(CTL) tests/test_scheduler tests/*.o

# ---- 单元测试 ----
TEST_CFLAGS := -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -Isrc

tests/test_scheduler: tests/test_scheduler.c \
		src/scheduler.c src/limiter.c src/util.c \
		src/config.h src/scheduler.h src/limiter.h src/control.h src/util.h
	$(CC) $(TEST_CFLAGS) -o $@ tests/test_scheduler.c src/scheduler.c src/limiter.c src/util.c

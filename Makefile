# 定义含有 Makefile 的子目录
SUBDIRS = mem Scheduling

# 定义伪目标，防止和文件名冲突
.PHONY: all clean $(SUBDIRS)

# 默认目标：编译所有子目录
all:
	@for dir in $(SUBDIRS); do \
		echo "Building in $$dir..."; \
		$(MAKE) -C $$dir; \
	done

# 清理目标：清理所有子目录
clean:
	@for dir in $(SUBDIRS); do \
		echo "Cleaning in $$dir..."; \
		$(MAKE) -C $$dir clean; \
	done
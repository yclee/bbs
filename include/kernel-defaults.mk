# 
# Copyright (C) 2006-2007 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

# For target profile selection - the default set
DEFAULT_PACKAGES:=base-files libgcc uclibc bridge busybox dnsmasq dropbear iptables mtd ppp ppp-mod-pppoe mtd kmod-ipt-nathelper
ifneq ($(KERNEL),2.4)
  DEFAULT_PACKAGES+=udevtrigger hotplug2
endif

KERNELNAME=
ifneq (,$(findstring x86,$(BOARD)))
  KERNELNAME="bzImage"
endif
ifneq (,$(findstring rdc,$(BOARD)))
  KERNELNAME="bzImage"
endif
ifneq (,$(findstring ppc,$(BOARD)))
  KERNELNAME="uImage"
endif
ifneq (,$(findstring avr32,$(BOARD)))
  KERNELNAME="uImage"
endif

KERNEL_MAKEOPTS := -C $(LINUX_DIR) \
	CROSS_COMPILE="$(KERNEL_CROSS)" \
	ARCH="$(LINUX_KARCH)" \
	CONFIG_SHELL="$(BASH)"

# defined in quilt.mk
Kernel/Patch:=$(Kernel/Patch/Default)
define Kernel/Prepare/Default
	bzcat $(DL_DIR)/$(LINUX_SOURCE) | $(TAR) -C $(KERNEL_BUILD_DIR) $(TAR_OPTIONS)
	$(Kernel/Patch)
endef

define Kernel/Configure/2.4
	$(SED) "s,\-mcpu=,\-mtune=,g;" $(LINUX_DIR)/arch/mips/Makefile
	$(MAKE) $(KERNEL_MAKEOPTS) CC="$(KERNEL_CC)" oldconfig include/linux/compile.h include/linux/version.h
	$(MAKE) $(KERNEL_MAKEOPTS) dep
endef
define Kernel/Configure/2.6
	-$(MAKE) $(KERNEL_MAKEOPTS) CC="$(KERNEL_CC)" oldconfig prepare scripts
endef
define Kernel/Configure/Default
	@if [ -f "./config/profile-$(PROFILE)" ]; then \
		$(SCRIPT_DIR)/config.pl '+' $(GENERIC_LINUX_CONFIG) '+' $(LINUX_CONFIG) ./config/profile-$(PROFILE) > $(LINUX_DIR)/.config; \
	else \
		$(SCRIPT_DIR)/config.pl '+' $(GENERIC_LINUX_CONFIG) $(LINUX_CONFIG) > $(LINUX_DIR)/.config; \
	fi
	$(call Kernel/Configure/$(KERNEL))
	rm -rf $(KERNEL_BUILD_DIR)/modules
	@rm -f $(BUILD_DIR)/linux
	ln -sf $(KERNEL_BUILD_DIR)/linux-$(LINUX_VERSION) $(BUILD_DIR)/linux
endef

define Kernel/CompileModules/Default
	rm -f $(LINUX_DIR)/vmlinux $(LINUX_DIR)/System.map
	$(MAKE) -j$(CONFIG_JLEVEL) $(KERNEL_MAKEOPTS) CC="$(KERNEL_CC)" modules
endef

ifeq ($(KERNEL),2.6)
  ifeq ($(CONFIG_TARGET_ROOTFS_INITRAMFS),y)
    define Kernel/SetInitramfs
		mv $(LINUX_DIR)/.config $(LINUX_DIR)/.config.old
		grep -v INITRAMFS $(LINUX_DIR)/.config.old > $(LINUX_DIR)/.config
		echo 'CONFIG_INITRAMFS_SOURCE="../../root"' >> $(LINUX_DIR)/.config
		echo 'CONFIG_INITRAMFS_ROOT_UID=0' >> $(LINUX_DIR)/.config
		echo 'CONFIG_INITRAMFS_ROOT_GID=0' >> $(LINUX_DIR)/.config
    endef
  else
    define Kernel/SetInitramfs
		mv $(LINUX_DIR)/.config $(LINUX_DIR)/.config.old
		grep -v INITRAMFS $(LINUX_DIR)/.config.old > $(LINUX_DIR)/.config
		rm -f $(BUILD_DIR)/root/init
		echo 'CONFIG_INITRAMFS_SOURCE=""' >> $(LINUX_DIR)/.config
    endef
  endif
endif
define Kernel/CompileImage/Default
	$(call Kernel/SetInitramfs)
	$(MAKE) -j$(CONFIG_JLEVEL) $(KERNEL_MAKEOPTS) CC="$(KERNEL_CC)" $(KERNELNAME)
	$(KERNEL_CROSS)objcopy -O binary -R .reginfo -R .note -R .comment -R .mdebug -S $(LINUX_DIR)/vmlinux $(LINUX_KERNEL)
	$(KERNEL_CROSS)objcopy -R .reginfo -R .note -R .comment -R .mdebug -S $(LINUX_DIR)/vmlinux $(KERNEL_BUILD_DIR)/vmlinux.elf
endef

define Kernel/Clean/Default
	rm -f $(LINUX_DIR)/.linux-compile
	rm -f $(KERNEL_BUILD_DIR)/linux-$(LINUX_VERSION)/.configured
	rm -f $(LINUX_KERNEL)
	$(MAKE) -C $(KERNEL_BUILD_DIR)/linux-$(LINUX_VERSION) clean
endef



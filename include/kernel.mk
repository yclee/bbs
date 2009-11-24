# 
# Copyright (C) 2006 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

ifeq ($(DUMP),1)
  KERNEL?=<KERNEL>
  BOARD?=<BOARD>
  LINUX_VERSION?=<LINUX_VERSION>
else
  include $(INCLUDE_DIR)/target.mk

  ifeq ($(KERNEL),2.6)
    LINUX_KMOD_SUFFIX=ko
  else
    LINUX_KMOD_SUFFIX=o
  endif

  ifneq (,$(findstring uml,$(BOARD)))
    KERNEL_CC:=$(HOSTCC)
    KERNEL_CROSS:=
  else
    KERNEL_CC:=$(TARGET_CC)
    KERNEL_CROSS:=$(TARGET_CROSS)
  endif

  KERNEL_PATCHVER:=$(shell echo $(LINUX_VERSION) | cut -d. -f1,2,3 | cut -d- -f1)
  PLATFORM_DIR := $(TOPDIR)/target/linux/$(BOARD)-$(KERNEL)
  PATCH_DIR := ./patches$(shell [ -d "./patches-$(KERNEL_PATCHVER)" ] && printf -- "-$(KERNEL_PATCHVER)" || true )
  KERNEL_BUILD_DIR:=$(BUILD_DIR)/linux-$(KERNEL)-$(BOARD)
  LINUX_DIR := $(KERNEL_BUILD_DIR)/linux-$(LINUX_VERSION)

  MODULES_SUBDIR:=lib/modules/$(LINUX_VERSION)
  TARGET_MODULES_DIR := $(LINUX_TARGET_DIR)/$(MODULES_SUBDIR)

  LINUX_KERNEL:=$(KERNEL_BUILD_DIR)/vmlinux

  LINUX_SOURCE:=linux-$(LINUX_VERSION).tar.bz2
  TESTING:=$(if $(findstring -rc,$(LINUX_VERSION)),/testing,)
  LINUX_SITE:=http://www.us.kernel.org/pub/linux/kernel/v$(KERNEL)$(TESTING) \
           http://www.us.kernel.org/pub/linux/kernel/v$(KERNEL)$(TESTING) \
           http://www.kernel.org/pub/linux/kernel/v$(KERNEL)$(TESTING) \
           http://www.de.kernel.org/pub/linux/kernel/v$(KERNEL)$(TESTING)

  PKG_BUILD_DIR ?= $(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)
endif
GENERIC_PLATFORM_DIR := $(TOPDIR)/target/linux/generic-$(KERNEL)
GENERIC_PATCH_DIR := $(GENERIC_PLATFORM_DIR)/patches$(shell [ -d "$(GENERIC_PLATFORM_DIR)/patches-$(KERNEL_PATCHVER)" ] && printf -- "-$(KERNEL_PATCHVER)" || true )


define KernelPackage/Defaults
  FILES:=
  KCONFIG:=m
  AUTOLOAD:=
endef

define ModuleAutoLoad
	export modules=; \
	add_module() { \
		mkdir -p $(2)/etc/modules.d; \
		( \
			for mod in $$$$$$$$2; do \
				getvar mod; \
			done \
		) > $(2)/etc/modules.d/$$$$$$$$1-$(1); \
		modules="$$$$$$$${modules:+$$$$$$$$modules }$$$$$$$$1-$(1)"; \
	}; \
	$(3) \
	if [ -n "$$$$$$$$modules" ]; then \
		mkdir -p $(2)/etc/modules.d; \
		echo "#!/bin/sh" > $(2)/CONTROL/postinst; \
		echo "[ -z \"\$$$$$$$$IPKG_INSTROOT\" ] || exit 0" >> $(2)/CONTROL/postinst; \
		echo ". /etc/functions.sh" >> $(2)/CONTROL/postinst; \
		echo "load_modules $$$$$$$$modules" >> $(2)/CONTROL/postinst; \
		chmod 0755 $(2)/CONTROL/postinst; \
	fi
endef
 

define KernelPackage
  NAME:=$(1)
  $(eval $(call Package/Default))
  $(eval $(call KernelPackage/Defaults))
  $(eval $(call KernelPackage/$(1)))
  $(eval $(call KernelPackage/$(1)/$(KERNEL)))
  $(eval $(call KernelPackage/$(1)/$(BOARD)-$(KERNEL)))

  define Package/kmod-$(1)
    TITLE:=$(TITLE)
    SECTION:=kernel
    CATEGORY:=Kernel modules
    DEFAULT:=$(KMOD_DEFAULT)
    DESCRIPTION:=$(DESCRIPTION)
    EXTRA_DEPENDS:=kernel (=$(LINUX_VERSION)-$(BOARD)-$(LINUX_RELEASE))
    $(call KernelPackage/$(1))
    $(call KernelPackage/$(1)/$(KERNEL))
    $(call KernelPackage/$(1)/$(BOARD)-$(KERNEL))
  endef

  ifeq ($(findstring m,$(KCONFIG)),m)
    ifneq ($(strip $(FILES)),)
      define Package/kmod-$(1)/install
		mkdir -p $$(1)/lib/modules/$(LINUX_VERSION)
		$(CP) -L $$(FILES) $$(1)/lib/modules/$(LINUX_VERSION)/
		$(call ModuleAutoLoad,$(1),$$(1),$(AUTOLOAD))
		$(call KernelPackage/$(1)/install,$$(1))
      endef
    endif
  endif
  $$(eval $$(call BuildPackage,kmod-$(1)))

  $$(IPKG_kmod-$(1)): $$(wildcard $$(FILES))
endef

define AutoLoad
  add_module $(1) "$(2)";
endef


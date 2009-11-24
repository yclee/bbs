#
# Copyright (C) 2006 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

define Profile/Texas
  NAME:=Texas Instruments WiFi (default)
  PACKAGES:=kmod-acx-mac80211
endef

define Profile/Texas/Description
	Package set compatible with hardware using Texas Instruments WiFi cards
endef
$(eval $(call Profile,Texas))


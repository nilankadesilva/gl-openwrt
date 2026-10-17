include $(TOPDIR)/rules.mk

PKG_NAME:=lcdd
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/lcdd
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Minimal DRM text console daemon for small SPI panels
  DEPENDS:=@TARGET_mediatek_filogic
endef

define Package/lcdd/description
 Reads Linux-console style ANSI text from a FIFO and draws it with an
 8x8 font into one DRM dumb buffer, flushing damage with DIRTYFB.
 Lets a MIPI DBI panel (GL-BE14000 LCD) show a dashboard without
 fbdev emulation or fbcon in the kernel. No libdrm dependency.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/lcdd $(PKG_BUILD_DIR)/lcdd.c
endef

define Package/lcdd/install
	$(INSTALL_DIR) $(1)/usr/bin $(1)/etc/init.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/lcdd $(1)/usr/bin/
	$(INSTALL_BIN) ./files/lcdd.init $(1)/etc/init.d/lcdd
endef

$(eval $(call BuildPackage,lcdd))

################################################################################
#
# inmp-test
#
################################################################################

INMP_TEST_VERSION = 1.0
INMP_TEST_SITE = $(BR2_EXTERNAL_INMP_PATH)/../test/harness
INMP_TEST_SITE_METHOD = local

define INMP_TEST_BUILD_CMDS
	$(MAKE) -C $(@D) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" inmpTest
endef

define INMP_TEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/inmpTest $(TARGET_DIR)/usr/bin/inmpTest
endef

$(eval $(generic-package))

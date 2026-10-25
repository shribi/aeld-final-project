################################################################################
#
# inmp-module
#
################################################################################

INMP_MODULE_VERSION = 1.0
INMP_MODULE_SITE = $(BR2_EXTERNAL_INMP_PATH)/../inmp-module
INMP_MODULE_SITE_METHOD = local
INMP_MODULE_LICENSE = GPL-2.0

$(eval $(kernel-module))
$(eval $(generic-package))

// SPDX-License-Identifier: GPL-2.0+
// Copyright 2018 IBM Corporation

#include <linux/regmap.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_probe_helper.h>

#include "aspeed_gfx.h"

static int aspeed_gfx_get_modes(struct drm_connector *connector)
{
	struct aspeed_gfx *priv = container_of(connector, struct aspeed_gfx, connector);

	if(priv->version == GFX_AST2600) {
		u32 clk_src;
		regmap_read(priv->scu, 0x300, &clk_src);
		if (((clk_src >> 8) & 0x7) == 0x2)
			//usb 40Mhz
			return drm_add_modes_noedid(connector, 800, 600);
		else if (((clk_src >> 8) & 0x7) == 0x7)
			//hpll div 16 = 75Mhz
			return drm_add_modes_noedid(connector, 1024, 768);
		else if (((clk_src >> 8) & 0x7) == 0x4)
			//dp div2 = 135Mhz
			return drm_add_modes_noedid(connector, 1280, 1024);
		else {
			printk("unknow clk source \n");
			return drm_add_modes_noedid(connector, 800, 600);
		}
	} else
		return drm_add_modes_noedid(connector, 800, 600);
}

static const struct
drm_connector_helper_funcs aspeed_gfx_connector_helper_funcs = {
	.get_modes = aspeed_gfx_get_modes,
};

static const struct drm_connector_funcs aspeed_gfx_connector_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

int aspeed_gfx_create_output(struct drm_device *drm)
{
	struct aspeed_gfx *priv = drm->dev_private;
	int ret;

	priv->connector.dpms = DRM_MODE_DPMS_OFF;
	priv->connector.polled = 0;
	drm_connector_helper_add(&priv->connector,
				 &aspeed_gfx_connector_helper_funcs);
	ret = drm_connector_init(drm, &priv->connector,
				 &aspeed_gfx_connector_funcs,
				 DRM_MODE_CONNECTOR_Unknown);
	return ret;
}

#include "bake2/plugin.h"

typedef int (*b2_plugin_init_t)(ecs_world_t *world, ecs_entity_t project_entity);

int b2_plugin_load_for_project(b2_context_t *ctx, const b2_project_cfg_t *cfg) {
    for (int32_t i = 0; i < cfg->plugins.count; i++) {
        const char *plugin = cfg->plugins.items[i];
        ecs_os_dl_t dl = ecs_os_dlopen(plugin);
        if (!dl) {
            B2_ERR("failed to load plugin %s", plugin);
            return -1;
        }

        b2_plugin_init_t init = (b2_plugin_init_t)ecs_os_dlproc(dl, "b2_plugin_init");
        if (!init) {
            B2_ERR("plugin %s does not export b2_plugin_init", plugin);
            ecs_os_dlclose(dl);
            return -1;
        }

        if (init(ctx->world, 0) != 0) {
            B2_ERR("plugin init failed for %s", plugin);
            ecs_os_dlclose(dl);
            return -1;
        }
    }

    return 0;
}

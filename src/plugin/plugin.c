#include "bake2/plugin.h"

typedef int (*bake_plugin_init_t)(ecs_world_t *world, ecs_entity_t project_entity);

int bake_plugin_load_for_project(bake_context_t *ctx, const bake_project_cfg_t *cfg) {
    for (int32_t i = 0; i < cfg->plugins.count; i++) {
        const char *plugin = cfg->plugins.items[i];
        ecs_os_dl_t dl = ecs_os_dlopen(plugin);
        if (!dl) {
            B2_ERR("failed to load plugin %s", plugin);
            return -1;
        }

        bake_plugin_init_t init = (bake_plugin_init_t)ecs_os_dlproc(dl, "bake_plugin_init");
        if (!init) {
            B2_ERR("plugin %s does not export bake_plugin_init", plugin);
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

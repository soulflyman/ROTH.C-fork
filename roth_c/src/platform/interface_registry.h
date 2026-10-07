/* interface_registry.h — host side of the SDK plugin-to-plugin interface registry
 * (roth_interfaces_v1). The roth_interfaces_* symbols the generated host adapter table binds are
 * defined in interface_registry.c; this header carries only what the loader calls.
 */
#ifndef INTERFACE_REGISTRY_H
#define INTERFACE_REGISTRY_H

struct roth_api_v1;

/* Withdraw everything the plugin identified by api still has published. The loader calls this
 * right after the plugin's on_unload returns. */
void roth_interfaces_release_owner(const struct roth_api_v1 *api);

#endif /* INTERFACE_REGISTRY_H */

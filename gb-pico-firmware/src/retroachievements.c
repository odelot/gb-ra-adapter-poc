#include "retroachievements.h"
#include "rc_runtime_types.h"
#include "rc_client.h"
#include "rc_client_internal.h"

#include "rc_api_info.h"
#include "rc_api_runtime.h"
#include "rc_api_user.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "rc_version.h"


// RCHEEVOS_API

// Write log messages to the console
static void log_message(const char *message, const rc_client_t *client)
{
    printf("%s\n", message);
}

// Initialize the RetroAchievements client
rc_client_t* initialize_retroachievements_client(rc_client_t *g_client, rc_client_read_memory_func_t read_memory, rc_client_server_call_t server_call)
{
     // Create the client instance (using a global variable simplifies this example)
    g_client = rc_client_create(read_memory, server_call);

    // Provide a logging function to simplify debugging
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_VERBOSE, log_message);


    // Disable hardcore - if we goof something up in the implementation, we don't want our
    // account disabled for cheating.
    rc_client_set_hardcore_enabled(g_client, 0);
    return g_client;
}

void shutdown_retroachievements_client(rc_client_t *g_client)
{
    if (g_client)
    {
        // Release resources associated to the client instance
        rc_client_destroy(g_client);
        g_client = NULL;
    }
}


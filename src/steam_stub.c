#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trace_call(const char *name)
{
    const char *enabled = getenv("SWAPPER_TRACE_NATIVE");
    if (enabled == NULL || strcmp(enabled, "1") != 0)
        return;

    FILE *file = fopen("steam-trace.log", "a");
    if (file != NULL)
    {
        fprintf(file, "%s\n", name);
        fclose(file);
    }
}

int32_t SteamInternal_SteamAPI_Init(const char *interface_versions, void *error_message)
{
    (void)interface_versions;
    (void)error_message;
    trace_call("SteamInternal_SteamAPI_Init");
    return 2;
}

int32_t SteamAPI_InitFlat(void *error_message)
{
    (void)error_message;
    trace_call("SteamAPI_InitFlat");
    return 2;
}

bool SteamAPI_InitAnonymousUser(void)
{
    trace_call("SteamAPI_InitAnonymousUser");
    return false;
}

bool SteamAPI_RestartAppIfNecessary(uint32_t app_id)
{
    (void)app_id;
    trace_call("SteamAPI_RestartAppIfNecessary");
    return false;
}

bool SteamAPI_Init(void)
{
    trace_call("SteamAPI_Init");
    return false;
}

bool SteamAPI_InitSafe(void)
{
    trace_call("SteamAPI_InitSafe");
    return false;
}

void SteamAPI_Shutdown(void)
{
    trace_call("SteamAPI_Shutdown");
}

void SteamAPI_RunCallbacks(void)
{
    trace_call("SteamAPI_RunCallbacks");
}

bool SteamAPI_IsSteamRunning(void)
{
    trace_call("SteamAPI_IsSteamRunning");
    return false;
}

int SteamAPI_GetHSteamUser(void)
{
    trace_call("SteamAPI_GetHSteamUser");
    return 0;
}

int SteamAPI_GetHSteamPipe(void)
{
    trace_call("SteamAPI_GetHSteamPipe");
    return 0;
}

const char *SteamAPI_GetSteamInstallPath(void)
{
    trace_call("SteamAPI_GetSteamInstallPath");
    return "";
}

void *SteamInternal_CreateInterface(const char *version)
{
    (void)version;
    trace_call("SteamInternal_CreateInterface");
    return NULL;
}

void *SteamInternal_FindOrCreateUserInterface(int steam_user, const char *version)
{
    (void)steam_user;
    (void)version;
    trace_call("SteamInternal_FindOrCreateUserInterface");
    return NULL;
}

void *SteamInternal_FindOrCreateGameServerInterface(int steam_user, const char *version)
{
    (void)steam_user;
    (void)version;
    trace_call("SteamInternal_FindOrCreateGameServerInterface");
    return NULL;
}

void SteamInternal_ContextInit(void *context)
{
    (void)context;
    trace_call("SteamInternal_ContextInit");
}

bool SteamInternal_GameServer_Init_V2(
    uint32_t ip,
    uint16_t steam_port,
    uint16_t game_port,
    uint16_t query_port,
    int32_t server_mode,
    const char *version,
    void *error_message)
{
    (void)ip;
    (void)steam_port;
    (void)game_port;
    (void)query_port;
    (void)server_mode;
    (void)version;
    (void)error_message;
    trace_call("SteamInternal_GameServer_Init_V2");
    return false;
}

bool SteamEncryptedAppTicket_BDecryptTicket(
    const void *encrypted_ticket,
    uint32_t encrypted_ticket_size,
    void *decrypted_ticket,
    uint32_t *decrypted_ticket_size,
    const void *key,
    int key_size)
{
    (void)encrypted_ticket;
    (void)encrypted_ticket_size;
    (void)decrypted_ticket;
    (void)key;
    (void)key_size;
    trace_call("SteamEncryptedAppTicket_BDecryptTicket");

    if (decrypted_ticket_size != NULL)
        *decrypted_ticket_size = 0;

    return false;
}

uint32_t SteamEncryptedAppTicket_GetTicketSteamID(const void *ticket, uint32_t ticket_size, void *steam_id)
{
    (void)ticket;
    (void)ticket_size;
    (void)steam_id;
    trace_call("SteamEncryptedAppTicket_GetTicketSteamID");
    return 0;
}

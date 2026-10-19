#pragma once

/* Deployed netplay-lobby endpoint. Release builds bake it in via
 * -DSSB64_NETPLAY_SERVER=...; anything that needs the service URL without
 * pulling the netplay session layer (crash-bundle uploads, diagnostics)
 * includes this instead of netlobby.h. Keep the fallback literal in sync
 * with services/netplay-lobby's deploy target. */

#if defined(SSB64_NETPLAY_SERVER)
#define PORT_NETLOBBY_DEFAULT_SERVER SSB64_NETPLAY_SERVER
#else
#define PORT_NETLOBBY_DEFAULT_SERVER "wss://smash-netplay.jumpcircuit.workers.dev"
#endif

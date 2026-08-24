#ifndef XIAOV_OPENVELA_SERVICE_H
#define XIAOV_OPENVELA_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runs one manually triggered voice turn against the configured gateway.
 * The function returns after playback drains or a fatal adapter error occurs.
 */
int xv_service_run_once(const char *host, uint16_t port, const char *path,
                        bool use_tls);

/* Keeps the gateway and UI workers alive across turns. A touch-capable UI
 * starts turns on demand; the serial fallback starts one initial turn. */
int xv_service_run_daemon(const char *host, uint16_t port, const char *path,
                          bool use_tls);

#ifdef __cplusplus
}
#endif

#endif

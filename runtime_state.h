#ifndef RUNTIME_STATE_H
#define RUNTIME_STATE_H

#include <stddef.h> /* size_t, pentru dimensiuni in semnaturile API */

#include "proto.h" /* contextserver, pentru initializarea runtime-ului din configuratie */

/* initializeaza starea globala, pentru metrice + upload + coada */
int runtime_init(const struct contextserver *context);
/* elibereaza starea globala, pentru shutdown curat */
int runtime_destroy(void);

/* hooks de conectare/deconectare, pentru statistici clienti */
void runtime_remote_connected(void);
void runtime_remote_disconnected(void);
void runtime_admin_connected(void);
void runtime_admin_disconnected(void);
/* hooks de request, pentru contorizare trafic logic */
void runtime_remote_request(void);
void runtime_admin_request(void);
/* semnalizare shutdown cooperativ, pentru firele de server */
void runtime_request_shutdown(void);
int runtime_shutdown_requested(void);

/* API upload/result, pentru protocolul clientului ordinar */
int runtime_begin_upload(const char *composition_name, size_t total_bytes, char *error_text, size_t error_size);
int runtime_append_chunk(size_t chunk_index, const unsigned char *data, size_t chunk_bytes, char *error_text, size_t error_size);
int runtime_get_status(const char *composition_name, char *status_text, size_t status_size, size_t *received, size_t *expected);
int runtime_get_result(const char *composition_name, char *result_path, size_t path_size, size_t *result_size);
/* marcheaza bytes trimisi spre client, pentru statistici */
void runtime_mark_bytes_out(size_t bytes);

/* executa o comanda admin text, pentru raspunsurile canalului UNIX */
int runtime_admin_line(const struct contextserver *context, const char *command, char *output, size_t output_size);

#endif

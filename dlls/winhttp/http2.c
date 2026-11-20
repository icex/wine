/*
 * Generic HTTP/2 transport for Wine WinHTTP. LGPL-2.1-or-later.
 * Copyright (c) 2026 Xodus Nexus contributors
 * Uses nghttp2 (MIT) for HTTP/2 parsing, HPACK and flow control, and Wine's
 * existing Schannel/certificate verification and request task queues.
 * One request per connection initially; no shared stream state or retries.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "windef.h"
#include "winbase.h"
#include "ws2tcpip.h"
#include "winhttp.h"
#include "winhttp_private.h"
#include "wine/debug.h"
#include <nghttp2/nghttp2.h>

WINE_DEFAULT_DEBUG_CHANNEL(winhttp);

#define H2_WINDOW 65535
#define H2_HEADERS_LIMIT 65536

struct http2_connection
{
    nghttp2_session *session;
    struct netconn *connection;
    struct request *owner;
    int32_t stream;
    DWORD error, status;
    BOOL headers_ready, ended, response_installed;
    BYTE body[H2_WINDOW];
    size_t body_len, header_len;
    char headers[H2_HEADERS_LIMIT + 1];
    const BYTE *upload;
    size_t upload_len, upload_pos;
    UINT64 upload_total, upload_read;
};

static int on_header( nghttp2_session *session, const nghttp2_frame *frame,
                      const uint8_t *name, size_t namelen, const uint8_t *value,
                      size_t valuelen, uint8_t flags, void *data )
{
    struct http2_connection *h2 = data;
    (void)session; (void)flags;
    if (frame->hd.stream_id != h2->stream || frame->hd.type != NGHTTP2_HEADERS) return 0;
    /* Trailers do not replace the initial response status/headers. */
    if (h2->headers_ready) return 0;
    if (namelen == 7 && !memcmp( name, ":status", 7 ))
    {
        if (valuelen != 3 || value[0] < '1' || value[0] > '5' ||
            value[1] < '0' || value[1] > '9' || value[2] < '0' || value[2] > '9')
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        h2->status = (value[0] - '0') * 100 + (value[1] - '0') * 10 + value[2] - '0';
        return 0;
    }
    if (!namelen || name[0] == ':' || namelen + valuelen + 4 > H2_HEADERS_LIMIT - h2->header_len ||
        memchr( name, '\r', namelen ) || memchr( name, '\n', namelen ) ||
        memchr( value, '\r', valuelen ) || memchr( value, '\n', valuelen ))
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy( h2->headers + h2->header_len, name, namelen ); h2->header_len += namelen;
    memcpy( h2->headers + h2->header_len, ": ", 2 ); h2->header_len += 2;
    memcpy( h2->headers + h2->header_len, value, valuelen ); h2->header_len += valuelen;
    memcpy( h2->headers + h2->header_len, "\r\n", 2 ); h2->header_len += 2;
    h2->headers[h2->header_len] = 0;
    return 0;
}

static int on_frame( nghttp2_session *session, const nghttp2_frame *frame, void *data )
{
    struct http2_connection *h2 = data;
    (void)session;
    if (frame->hd.type == NGHTTP2_GOAWAY && frame->goaway.error_code)
        h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (frame->hd.stream_id == h2->stream && frame->hd.type == NGHTTP2_HEADERS && !h2->headers_ready)
    {
        if (h2->status >= 200) h2->headers_ready = TRUE;
        else if (h2->status >= 100 && h2->status != 101)
            h2->header_len = h2->status = 0;
        else h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    }
    return 0;
}

static int on_body( nghttp2_session *session, uint8_t flags, int32_t stream,
                    const uint8_t *data, size_t length, void *user )
{
    struct http2_connection *h2 = user;
    (void)session; (void)flags;
    if (stream != h2->stream) return NGHTTP2_ERR_CALLBACK_FAILURE;
    if (!h2->headers_ready || length > sizeof(h2->body) - h2->body_len)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    memcpy( h2->body + h2->body_len, data, length );
    h2->body_len += length;
    return 0;
}

static int on_close( nghttp2_session *session, int32_t stream, uint32_t error, void *data )
{
    struct http2_connection *h2 = data;
    (void)session;
    if (stream == h2->stream)
    {
        h2->ended = TRUE;
        if (error) h2->error = error == NGHTTP2_CANCEL ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    }
    return 0;
}

static ssize_t upload_read( nghttp2_session *session, int32_t stream, uint8_t *buffer,
                            size_t length, uint32_t *flags, nghttp2_data_source *source, void *data )
{
    struct http2_connection *h2 = data;
    size_t count = min( length, h2->upload_len - h2->upload_pos );
    (void)session; (void)stream; (void)source;
    if (count)
    {
        memcpy( buffer, h2->upload + h2->upload_pos, count );
        h2->upload_pos += count;
        h2->upload_read += count;
    }
    if (h2->upload_read == h2->upload_total) *flags |= NGHTTP2_DATA_FLAG_EOF;
    else if (!count) return NGHTTP2_ERR_DEFERRED;
    return count;
}

static DWORD flush_frames( struct http2_connection *h2 )
{
    const uint8_t *frame;
    nghttp2_ssize length;
    int sent;
    DWORD ret;
    if (InterlockedCompareExchange( &h2->owner->cancelled, 0, 0 )) return h2->error = ERROR_WINHTTP_OPERATION_CANCELLED;
    while ((length = nghttp2_session_mem_send2( h2->session, &frame )) > 0)
        if ((ret = netconn_send( h2->connection, frame, length, &sent, NULL ))) return h2->error = ret;
    if (length < 0) h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    return h2->error;
}

static DWORD receive_frames( struct http2_connection *h2 )
{
    BYTE input[16384];
    int count;
    nghttp2_ssize consumed;
    DWORD ret;
    DWORD timeout = 0;
    int option_size = sizeof(timeout);
    ULONGLONG started = GetTickCount64();
    if (h2->error) return h2->error;
    if ((ret = flush_frames( h2 ))) return ret;
    getsockopt( (SOCKET)h2->connection->socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, &option_size );
    /* Cancellable wait without a second worker or request-object lifetime. */
    while (!h2->connection->extra_len)
    {
        fd_set readable;
        struct timeval interval = {0, 100000};
        int ready;
        if (InterlockedCompareExchange( &h2->owner->cancelled, 0, 0 )) return h2->error = ERROR_WINHTTP_OPERATION_CANCELLED;
        FD_ZERO(&readable); FD_SET((SOCKET)h2->connection->socket, &readable);
        ready = select(0, &readable, NULL, NULL, &interval);
        if (ready > 0) break;
        if (ready < 0) return h2->error = ERROR_WINHTTP_CONNECTION_ERROR;
        if (timeout && GetTickCount64() - started >= timeout) return h2->error = ERROR_WINHTTP_TIMEOUT;
    }
    if ((ret = netconn_recv( h2->connection, input, sizeof(input), 0, &count ))) return h2->error = ret;
    if (!count) return h2->error = ERROR_WINHTTP_CONNECTION_ERROR;
    consumed = nghttp2_session_mem_recv2( h2->session, input, count );
    if (consumed != count) return h2->error = ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    return flush_frames( h2 );
}

void http2_destroy( struct netconn *connection )
{
    struct http2_connection *h2 = connection->http2_state;
    if (!h2) return;
    nghttp2_session_del( h2->session );
    free( h2 );
    connection->http2_state = NULL;
}

static DWORD initialize( struct request *request )
{
    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_option *options = NULL;
    struct http2_connection *h2;
    int result;
    nghttp2_settings_entry settings[] = {
        { NGHTTP2_SETTINGS_ENABLE_PUSH, 0 },
        { NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, H2_HEADERS_LIMIT },
        { NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, H2_WINDOW }
    };
    if (!(h2 = calloc( 1, sizeof(*h2) ))) return ERROR_OUTOFMEMORY;
    h2->connection = request->netconn;
    h2->owner = request; /* Native WinHTTP task owns its object reference. */
    if (nghttp2_session_callbacks_new( &callbacks ) || nghttp2_option_new( &options )) goto failed;
    nghttp2_session_callbacks_set_on_header_callback( callbacks, on_header );
    nghttp2_session_callbacks_set_on_frame_recv_callback( callbacks, on_frame );
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback( callbacks, on_body );
    nghttp2_session_callbacks_set_on_stream_close_callback( callbacks, on_close );
    nghttp2_option_set_no_auto_window_update( options, 1 );
    result = nghttp2_session_client_new2( &h2->session, callbacks, h2, options );
    nghttp2_session_callbacks_del( callbacks ); callbacks = NULL;
    nghttp2_option_del( options ); options = NULL;
    if (result) goto failed;
    if (nghttp2_submit_settings( h2->session, NGHTTP2_FLAG_NONE, settings, ARRAY_SIZE(settings) )) goto failed;
    request->netconn->http2_state = h2;
    return ERROR_SUCCESS;
failed:
    if (callbacks) nghttp2_session_callbacks_del( callbacks );
    if (options) nghttp2_option_del( options );
    if (h2->session) nghttp2_session_del( h2->session );
    free( h2 );
    return ERROR_OUTOFMEMORY;
}

static char *utf8( const WCHAR *value )
{
    int length = WideCharToMultiByte( CP_UTF8, 0, value, -1, NULL, 0, NULL, NULL );
    char *result;
    if (!length || !(result = malloc( length ))) return NULL;
    WideCharToMultiByte( CP_UTF8, 0, value, -1, result, length, NULL, NULL );
    return result;
}

static BOOL hop_header( struct request *request, const WCHAR *name )
{
    DWORD i;
    const WCHAR *cursor, *end, *last;
    if (!wcsicmp( name, L"Connection" ) || !wcsicmp( name, L"Proxy-Connection" ) ||
        !wcsicmp( name, L"Keep-Alive" ) || !wcsicmp( name, L"Upgrade" ) ||
        !wcsicmp( name, L"Transfer-Encoding" ) || !wcsicmp( name, L"Host" )) return TRUE;
    for (i = 0; i < request->num_headers; i++)
    {
        if (!request->headers[i].is_request || wcsicmp( request->headers[i].field, L"Connection" )) continue;
        for (cursor = request->headers[i].value; *cursor; cursor = *end ? end + 1 : end)
        {
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            end = wcschr( cursor, ',' ); if (!end) end = cursor + wcslen( cursor );
            last = end;
            while (last > cursor && (last[-1] == ' ' || last[-1] == '\t')) last--;
            if (wcslen( name ) == last - cursor && !wcsnicmp( name, cursor, last - cursor )) return TRUE;
        }
    }
    return FALSE;
}

static BOOL add_nv( nghttp2_nv *nv, const char *name, const char *value )
{
    nv->name = (uint8_t *)strdup( name ); nv->value = (uint8_t *)strdup( value );
    if (!nv->name || !nv->value) return FALSE;
    nv->namelen = strlen(name); nv->valuelen = strlen(value);
    nv->flags = NGHTTP2_NV_FLAG_NO_INDEX; /* Do not retain sensitive headers in dynamic tables. */
    return TRUE;
}

DWORD http2_send_request( struct request *request, const char *path, void *optional, DWORD length, UINT64 total )
{
    struct http2_connection *h2;
    nghttp2_nv *headers;
    nghttp2_data_provider provider = {{0}, upload_read};
    DWORD ret, i, count = 0;
    const WCHAR *authority = request->connect->hostname;
    char *method = NULL, *host = NULL;
    if (length > total) return ERROR_INVALID_PARAMETER;
    /* An undrained old stream must not be reused or silently replayed. */
    if (request->netconn->http2_state) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if ((ret = initialize( request ))) return ret;
    h2 = request->netconn->http2_state;
    h2->upload_total = total;
    h2->upload = optional; h2->upload_len = length;
    if (!(headers = calloc( request->num_headers + 4, sizeof(*headers) ))) return ERROR_OUTOFMEMORY;
    for (i = 0; i < request->num_headers; i++)
        if (request->headers[i].is_request && !wcsicmp( request->headers[i].field, L"Host" )) authority = request->headers[i].value;
    ret = ERROR_OUTOFMEMORY;
    if (!(method = utf8( request->verb )) || !(host = utf8( authority ))) goto done;
    /* The pseudo-headers must precede regular request headers. */
#define NV(n,v) do { if (!add_nv( headers + count++, n, v )) goto done; } while (0)
    NV(":method", method); NV(":scheme", "https"); NV(":authority", host); NV(":path", path);
    for (i = 0; i < request->num_headers; i++)
    {
        char *name, *value;
        BOOL ok;
        if (!request->headers[i].is_request || hop_header( request, request->headers[i].field )) continue;
        name = utf8( request->headers[i].field ); value = utf8( request->headers[i].value );
        if (!name || !value) { free(name); free(value); goto done; }
        for (char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
        ok = add_nv( headers + count++, name, value );
        free(name); free(value);
        if (!ok) goto done;
    }
#undef NV
    h2->stream = nghttp2_submit_request( h2->session, NULL, headers, count, total ? &provider : NULL, NULL );
    if (h2->stream < 0) { ret = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; goto done; }
    ret = flush_frames( h2 );
    while (!ret && h2->upload_pos < h2->upload_len && !h2->ended) ret = receive_frames( h2 );
    if (!ret && h2->upload_pos < h2->upload_len) ret = ERROR_WINHTTP_CONNECTION_ERROR;
    h2->upload = NULL; h2->upload_len = h2->upload_pos = 0;
done:
    for (i = 0; i < count; i++) { free( headers[i].name ); free( headers[i].value ); }
    free(headers); free(method); free(host);
    return ret;
}

DWORD http2_read_reply( struct request *request )
{
    struct http2_connection *h2 = request->netconn->http2_state;
    DWORD ret;
    char prefix[32];
    WCHAR *raw, *cursor, *version, *text, status[4];
    int length, prefix_len;
    if (!h2) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (h2->response_installed) return h2->error;
    while (!h2->headers_ready && !h2->ended)
        if ((ret = receive_frames( h2 ))) return ret;
    if (h2->error) return h2->error;
    if (!h2->headers_ready) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    prefix_len = snprintf( prefix, sizeof(prefix), "HTTP/2.0 %lu\r\n", h2->status );
    length = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, h2->headers, h2->header_len, NULL, 0 );
    if (h2->header_len && !length) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    if (!(raw = calloc( prefix_len + length + 3, sizeof(WCHAR) ))) return ERROR_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, 0, prefix, prefix_len, raw, prefix_len );
    if (length) MultiByteToWideChar( CP_UTF8, 0, h2->headers, h2->header_len, raw + prefix_len, length );
    memcpy( raw + prefix_len + length, L"\r\n", sizeof(L"\r\n") );
    swprintf( status, ARRAY_SIZE(status), L"%lu", h2->status );
    TRACE( "version [HTTP/2.0] status code [%s] status text []\n", debugstr_w(status) );
    ret = process_header( request, L"Status", status, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE, FALSE );
    for (cursor = raw + prefix_len; !ret && *cursor && *cursor != '\r'; )
    {
        WCHAR *colon = wcschr(cursor, ':'), *end = wcsstr(cursor, L"\r\n");
        if (!colon || !end || colon > end) { ret = ERROR_WINHTTP_INVALID_SERVER_RESPONSE; break; }
        *colon = 0; *end = 0;
        ret = process_header( request, cursor, colon + 2, WINHTTP_ADDREQ_FLAG_ADD, FALSE );
        *colon = ':'; *end = '\r'; cursor = end + 2;
    }
    if (ret) { free(raw); return ret; }
    version = wcsdup(L"HTTP/2.0"); text = wcsdup(L"");
    if (!version || !text) { free(version); free(text); free(raw); return ERROR_OUTOFMEMORY; }
    free(request->version); request->version = version;
    free(request->status_text); request->status_text = text;
    free(request->raw_headers); request->raw_headers = raw;
    request->read_reply_len = prefix_len + h2->header_len + 2;
    h2->response_installed = TRUE;
    return ERROR_SUCCESS;
}

DWORD http2_read_data( struct netconn *connection, void *buffer, DWORD length, int *read )
{
    struct http2_connection *h2 = connection->http2_state;
    DWORD ret;
    *read = 0;
    if (!h2) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    if (h2->error) return h2->error;
    if (!length) return ERROR_SUCCESS;
    while (!h2->body_len && !h2->ended) if ((ret = receive_frames( h2 ))) return ret;
    if (h2->error) return h2->error;
    *read = min( length, h2->body_len );
    if (*read)
    {
        memcpy( buffer, h2->body, *read ); h2->body_len -= *read;
        memmove( h2->body, h2->body + *read, h2->body_len );
        if (nghttp2_session_consume( h2->session, h2->stream, *read )) return ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
        return flush_frames( h2 );
    }
    return ERROR_SUCCESS;
}

DWORD http2_available( struct netconn *connection )
{
    return connection->http2_state ? connection->http2_state->body_len : 0;
}

DWORD http2_write_data( struct netconn *connection, const void *buffer, DWORD length, int *written )
{
    struct http2_connection *h2 = connection->http2_state;
    DWORD ret;
    *written = 0;
    if (!h2 || h2->ended || length > h2->upload_total - h2->upload_read) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    h2->upload = buffer; h2->upload_len = length; h2->upload_pos = 0;
    if (nghttp2_session_resume_data( h2->session, h2->stream )) return ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
    ret = flush_frames( h2 );
    while (!ret && h2->upload_pos < length && !h2->ended) ret = receive_frames( h2 );
    *written = h2->upload_pos;
    h2->upload = NULL; h2->upload_len = h2->upload_pos = 0;
    if (!ret && *written != length) ret = ERROR_WINHTTP_CONNECTION_ERROR;
    return ret;
}

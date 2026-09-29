#include "signaling_client.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "peer.h"

#define SIGNALING_MAX_RESPONSE 32768
#define SIGNALING_POLL_INTERVAL_MS 100
#define H264_FMTP_PREFIX "a=fmtp:96 "
#define H264_PACKETIZATION_MODE "packetization-mode=1;"
#define SDP_BUNDLE_PREFIX "a=group:BUNDLE"
#define SDP_CANDIDATE_PREFIX "a=candidate:"
#define SDP_MID_PREFIX "a=mid:"
#define SDP_MAX_LINES 256
#define SDP_MAX_MEDIA_SECTIONS 8
#define SDP_MAX_CANDIDATES 32

typedef struct HttpResponse {
  int status;
  size_t body_len;
  char body[SIGNALING_MAX_RESPONSE];
} HttpResponse;

typedef struct SdpLine {
  const char* start;
  size_t length;
  size_t raw_length;
  int media_index;
} SdpLine;

typedef struct SdpMediaSection {
  size_t mid_line;
  int is_bundle_member;
  int has_h264_payload_96;
} SdpMediaSection;

typedef struct SdpDocument {
  SdpLine lines[SDP_MAX_LINES];
  SdpMediaSection media_sections[SDP_MAX_MEDIA_SECTIONS];
  size_t candidate_lines[SDP_MAX_CANDIDATES];
  size_t line_count;
  size_t media_count;
  size_t candidate_count;
  size_t bundle_line;
  size_t bundle_mid_count;
  const char* line_ending;
  size_t line_ending_length;
} SdpDocument;

typedef struct SdpOutput {
  char* data;
  size_t capacity;
  size_t length;
} SdpOutput;

static int sdp_line_starts_with(const SdpLine* line, const char* prefix) {
  size_t prefix_length = strlen(prefix);
  return line->length >= prefix_length &&
         memcmp(line->start, prefix, prefix_length) == 0;
}

static int sdp_line_contains(const SdpLine* line, const char* value) {
  size_t value_length = strlen(value);
  size_t offset;

  if (value_length == 0 || line->length < value_length) {
    return 0;
  }
  for (offset = 0; offset + value_length <= line->length; ++offset) {
    if (memcmp(line->start + offset, value, value_length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int sdp_output_append(SdpOutput* output,
                             const char* data,
                             size_t length) {
  if (length > output->capacity - output->length - 1) {
    return -1;
  }
  memcpy(output->data + output->length, data, length);
  output->length += length;
  output->data[output->length] = '\0';
  return 0;
}

static size_t sdp_bundle_mid_count(const SdpLine* bundle_line) {
  const char* cursor = bundle_line->start + strlen(SDP_BUNDLE_PREFIX);
  const char* end = bundle_line->start + bundle_line->length;
  size_t count = 0;

  while (cursor < end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
      ++cursor;
    }
    if (cursor == end) {
      break;
    }
    ++count;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') {
      ++cursor;
    }
  }
  return count;
}

static int sdp_bundle_contains_mid(const SdpLine* bundle_line,
                                   const SdpLine* mid_line) {
  const char* mid = mid_line->start + strlen(SDP_MID_PREFIX);
  size_t mid_length = mid_line->length - strlen(SDP_MID_PREFIX);
  const char* cursor = bundle_line->start + strlen(SDP_BUNDLE_PREFIX);
  const char* end = bundle_line->start + bundle_line->length;

  while (cursor < end) {
    const char* token;
    size_t token_length;

    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
      ++cursor;
    }
    token = cursor;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') {
      ++cursor;
    }
    token_length = (size_t)(cursor - token);
    if (token_length == mid_length && memcmp(token, mid, mid_length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int sdp_candidate_already_collected(const SdpDocument* document,
                                           size_t line_index) {
  const SdpLine* candidate = &document->lines[line_index];
  size_t index;

  for (index = 0; index < document->candidate_count; ++index) {
    const SdpLine* collected =
        &document->lines[document->candidate_lines[index]];
    if (candidate->length == collected->length &&
        memcmp(candidate->start, collected->start, candidate->length) == 0) {
      return 1;
    }
  }
  return 0;
}

static int sdp_parse(const char* description, SdpDocument* document) {
  const char* cursor = description;
  int media_index = -1;

  memset(document, 0, sizeof(*document));
  document->bundle_line = (size_t)-1;
  document->line_ending = "\r\n";
  document->line_ending_length = 2;

  while (*cursor) {
    const char* newline;
    const char* content_end;
    const char* raw_end;
    SdpLine* line;

    if (document->line_count == SDP_MAX_LINES) {
      return -1;
    }
    newline = strchr(cursor, '\n');
    raw_end = newline ? newline + 1 : cursor + strlen(cursor);
    content_end = newline ? newline : raw_end;
    if (content_end > cursor && content_end[-1] == '\r') {
      --content_end;
    }

    line = &document->lines[document->line_count];
    line->start = cursor;
    line->length = (size_t)(content_end - cursor);
    line->raw_length = (size_t)(raw_end - cursor);

    if (line->length >= 2 && memcmp(line->start, "m=", 2) == 0) {
      if (document->media_count == SDP_MAX_MEDIA_SECTIONS) {
        return -1;
      }
      media_index = (int)document->media_count++;
      document->media_sections[media_index].mid_line = (size_t)-1;
    }
    line->media_index = media_index;

    if (media_index < 0 && sdp_line_starts_with(line, SDP_BUNDLE_PREFIX)) {
      if (document->bundle_line != (size_t)-1) {
        return -1;
      }
      document->bundle_line = document->line_count;
      document->bundle_mid_count = sdp_bundle_mid_count(line);
    } else if (media_index >= 0 &&
               sdp_line_starts_with(line, SDP_MID_PREFIX)) {
      if (document->media_sections[media_index].mid_line != (size_t)-1) {
        return -1;
      }
      document->media_sections[media_index].mid_line = document->line_count;
    } else if (media_index >= 0 &&
               sdp_line_starts_with(line, "a=rtpmap:96 H264/90000")) {
      document->media_sections[media_index].has_h264_payload_96 = 1;
    }

    if (newline && document->line_count == 0) {
      document->line_ending = content_end < newline ? "\r\n" : "\n";
      document->line_ending_length = content_end < newline ? 2 : 1;
    }
    ++document->line_count;
    cursor = raw_end;
  }
  return document->line_count > 0 ? 0 : -1;
}

static int sdp_collect_bundle_candidates(SdpDocument* document) {
  const SdpLine* bundle_line;
  size_t media_index;
  size_t line_index;

  if (document->bundle_line == (size_t)-1 ||
      document->bundle_mid_count <= 1) {
    return 0;
  }
  bundle_line = &document->lines[document->bundle_line];
  for (media_index = 0; media_index < document->media_count; ++media_index) {
    SdpMediaSection* media = &document->media_sections[media_index];
    if (media->mid_line != (size_t)-1 &&
        sdp_bundle_contains_mid(bundle_line,
                                &document->lines[media->mid_line])) {
      media->is_bundle_member = 1;
    }
  }

  for (line_index = 0; line_index < document->line_count; ++line_index) {
    const SdpLine* line = &document->lines[line_index];
    if (line->media_index < 0 ||
        !document->media_sections[line->media_index].is_bundle_member ||
        !sdp_line_starts_with(line, SDP_CANDIDATE_PREFIX) ||
        sdp_candidate_already_collected(document, line_index)) {
      continue;
    }
    if (document->candidate_count == SDP_MAX_CANDIDATES) {
      return -1;
    }
    document->candidate_lines[document->candidate_count++] = line_index;
  }
  return 0;
}

static int sdp_append_line(SdpOutput* output,
                           const SdpDocument* document,
                           size_t line_index) {
  const SdpLine* line = &document->lines[line_index];
  const SdpMediaSection* media =
      line->media_index >= 0
          ? &document->media_sections[line->media_index]
          : NULL;
  size_t prefix_length = strlen(H264_FMTP_PREFIX);

  if (media && media->has_h264_payload_96 &&
      sdp_line_starts_with(line, H264_FMTP_PREFIX) &&
      !sdp_line_contains(line, "packetization-mode=")) {
    if (sdp_output_append(output, line->start, prefix_length) != 0 ||
        sdp_output_append(output,
                          H264_PACKETIZATION_MODE,
                          strlen(H264_PACKETIZATION_MODE)) != 0 ||
        sdp_output_append(output,
                          line->start + prefix_length,
                          line->raw_length - prefix_length) != 0) {
      return -1;
    }
    return 0;
  }
  return sdp_output_append(output, line->start, line->raw_length);
}

static int sdp_append_candidates(SdpOutput* output,
                                 const SdpDocument* document) {
  size_t candidate_index;

  for (candidate_index = 0;
       candidate_index < document->candidate_count;
       ++candidate_index) {
    const SdpLine* candidate =
        &document->lines[document->candidate_lines[candidate_index]];
    if (sdp_output_append(output, candidate->start, candidate->length) != 0 ||
        sdp_output_append(output,
                          document->line_ending,
                          document->line_ending_length) != 0) {
      return -1;
    }
  }
  return 0;
}

static int prepare_local_description(const char* description,
                                     char* output,
                                     size_t output_size) {
  SdpDocument document;
  SdpOutput sdp_output;
  size_t line_index;

  if (!description || !output || output_size == 0) {
    return -1;
  }
  if (sdp_parse(description, &document) != 0 ||
      sdp_collect_bundle_candidates(&document) != 0) {
    return -1;
  }

  sdp_output.data = output;
  sdp_output.capacity = output_size;
  sdp_output.length = 0;
  output[0] = '\0';

  for (line_index = 0; line_index < document.line_count; ++line_index) {
    const SdpLine* line = &document.lines[line_index];
    const SdpMediaSection* media =
        line->media_index >= 0
            ? &document.media_sections[line->media_index]
            : NULL;

    if (media && media->is_bundle_member &&
        sdp_line_starts_with(line, SDP_CANDIDATE_PREFIX)) {
      continue;
    }
    if (sdp_append_line(&sdp_output, &document, line_index) != 0) {
      return -1;
    }
    if (media && media->is_bundle_member &&
        media->mid_line == line_index &&
        sdp_append_candidates(&sdp_output, &document) != 0) {
      return -1;
    }
  }
  return 0;
}

static unsigned long long signaling_now_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL;
}

static int parse_url(SignalingClient* client, const char* url) {
  const char* rest;
  const char* path_start;
  const char* query_start;
  char authority[256];
  size_t authority_len;
  const char* port_start;

  if (!client || !url || strncmp(url, "http://", 7) != 0) {
    fprintf(stderr, "[SIGNALING] only http:// URLs are supported\n");
    return -1;
  }
  rest = url + 7;
  query_start = strchr(rest, '?');
  path_start = strchr(rest, '/');
  if (!path_start || (query_start && query_start < path_start)) {
    path_start = query_start;
  }
  authority_len = path_start ? (size_t)(path_start - rest) : strlen(rest);
  if (authority_len == 0 || authority_len >= sizeof(authority)) {
    return -1;
  }
  memcpy(authority, rest, authority_len);
  authority[authority_len] = '\0';

  port_start = strchr(authority, ':');
  if (port_start) {
    size_t host_len = (size_t)(port_start - authority);
    if (host_len == 0 || host_len >= sizeof(client->host)) {
      return -1;
    }
    memcpy(client->host, authority, host_len);
    client->host[host_len] = '\0';
    client->port = atoi(port_start + 1);
  } else {
    snprintf(client->host, sizeof(client->host), "%s", authority);
    client->port = 80;
  }
  if (client->port <= 0 || client->port > 65535) {
    return -1;
  }

  if (!path_start) {
    snprintf(client->base_path, sizeof(client->base_path), "/");
  } else if (*path_start == '?') {
    snprintf(client->base_path, sizeof(client->base_path), "/%s", path_start);
  } else {
    snprintf(client->base_path, sizeof(client->base_path), "%s", path_start);
  }
  return 0;
}

static void build_role_path(char* output, size_t output_size, const char* base_path, const char* role) {
  const char* query = strchr(base_path, '?');
  size_t base_len = query ? (size_t)(query - base_path) : strlen(base_path);
  if (base_len == 0) {
    snprintf(output, output_size, "/%s%s", role, query ? query : "");
  } else if (base_path[base_len - 1] == '/') {
    snprintf(output, output_size, "%.*s%s%s", (int)base_len, base_path, role, query ? query : "");
  } else {
    snprintf(output, output_size, "%.*s/%s%s", (int)base_len, base_path, role, query ? query : "");
  }
}

static int send_all(int socket_fd, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    ssize_t count = send(socket_fd, data + sent, length - sent, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return -1;
    }
    sent += (size_t)count;
  }
  return 0;
}

static int open_connection(const SignalingClient* client) {
  char port_text[16];
  struct addrinfo hints;
  struct addrinfo* addresses = NULL;
  struct addrinfo* address;
  int socket_fd = -1;

  snprintf(port_text, sizeof(port_text), "%d", client->port);
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(client->host, port_text, &hints, &addresses) != 0) {
    return -1;
  }
  for (address = addresses; address; address = address->ai_next) {
    socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd < 0) {
      continue;
    }
    if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
      break;
    }
    close(socket_fd);
    socket_fd = -1;
  }
  freeaddrinfo(addresses);
  return socket_fd;
}

static int http_request(const SignalingClient* client,
                        const char* method,
                        const char* path,
                        const char* body,
                        HttpResponse* response) {
  char request[2048];
  char raw[SIGNALING_MAX_RESPONSE];
  size_t body_size = body ? strlen(body) : 0;
  size_t received = 0;
  size_t header_size = 0;
  size_t expected_body = 0;
  int socket_fd;
  int content_length_found = 0;

  if (!client || !method || !path || !response || body_size > SIGNALING_MAX_RESPONSE / 2) {
    return -1;
  }
  socket_fd = open_connection(client);
  if (socket_fd < 0) {
    return -1;
  }
  int request_size = snprintf(request,
                              sizeof(request),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s:%d\r\n"
                              "Content-Type: application/sdp\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              method,
                              path,
                              client->host,
                              client->port,
                              body_size);
  if (request_size < 0 || (size_t)request_size >= sizeof(request) ||
      send_all(socket_fd, request, (size_t)request_size) != 0 ||
      (body_size > 0 && send_all(socket_fd, body, body_size) != 0)) {
    close(socket_fd);
    return -1;
  }

  while (received + 1 < sizeof(raw)) {
    ssize_t count = recv(socket_fd, raw + received, sizeof(raw) - received - 1, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    received += (size_t)count;
    raw[received] = '\0';
    char* header_end = strstr(raw, "\r\n\r\n");
    if (!header_end) {
      continue;
    }
    header_size = (size_t)(header_end - raw) + 4;
    if (!content_length_found) {
      char* content_length = strstr(raw, "Content-Length:");
      if (content_length && content_length < header_end) {
        expected_body = (size_t)strtoul(content_length + strlen("Content-Length:"), NULL, 10);
        content_length_found = 1;
      }
    }
    if (content_length_found && received >= header_size + expected_body) {
      break;
    }
  }
  close(socket_fd);

  if (header_size == 0) {
    return -1;
  }
  response->status = 0;
  sscanf(raw, "HTTP/%*s %d", &response->status);
  response->body_len = received > header_size ? received - header_size : 0;
  if (response->body_len > sizeof(response->body) - 1) {
    response->body_len = sizeof(response->body) - 1;
  }
  memcpy(response->body, raw + header_size, response->body_len);
  response->body[response->body_len] = '\0';
  return response->status > 0 ? 0 : -1;
}

static void onicecandidate(char* description, void* user_data) {
  SignalingClient* client = (SignalingClient*)user_data;
  HttpResponse response;
  char local_description[SIGNALING_MAX_RESPONSE];
  const char* path;

  if (!client || !description) {
    return;
  }
  if (prepare_local_description(description,
                                local_description,
                                sizeof(local_description)) != 0) {
    fprintf(stderr, "[SIGNALING] failed to prepare local description\n");
    return;
  }
  path = client->is_offerer ? client->offer_path : client->answer_path;
  fprintf(stderr, "[SIGNALING] POST http://%s:%d%s body_len=%zu\n",
          client->host, client->port, path, strlen(local_description));
  int http_ret = http_request(client, "POST", path, local_description, &response);
  if (http_ret == 0 && response.status == 201) {
    if (client->is_offerer) {
      client->offer_sent = 1;
    } else {
      client->answer_sent = 1;
    }
    printf("[SIGNALING] sent %s\n", client->is_offerer ? "offer" : "answer");
  } else {
    fprintf(stderr, "[SIGNALING] failed to send %s (http_ret=%d, status=%d, host=%s:%d)\n",
            client->is_offerer ? "offer" : "answer",
            http_ret, response.status, client->host, client->port);
  }
}

int signaling_client_connect(SignalingClient* client,
                             const char* url,
                             PeerConnection* pc,
                             int is_offerer) {
  if (!client || !pc || parse_url(client, url) != 0) {
    return -1;
  }
  client->pc = pc;
  client->is_offerer = is_offerer ? 1 : 0;
  build_role_path(client->offer_path, sizeof(client->offer_path), client->base_path, "offer");
  build_role_path(client->answer_path, sizeof(client->answer_path), client->base_path, "answer");
  peer_connection_onicecandidate(pc, onicecandidate);
  if (client->is_offerer) {
    peer_connection_create_offer(pc);
    if (!client->offer_sent) {
      return -1;
    }
  }
  client->next_poll_ms = signaling_now_ms();
  return 0;
}

int signaling_client_loop(SignalingClient* client) {
  HttpResponse response;
  const char* path;

  if (!client || !client->pc || signaling_now_ms() < client->next_poll_ms) {
    return 0;
  }
  client->next_poll_ms = signaling_now_ms() + SIGNALING_POLL_INTERVAL_MS;
  if (client->is_offerer) {
    if (client->answer_received || !client->offer_sent) {
      return 0;
    }
    path = client->answer_path;
  } else {
    if (client->offer_received) {
      return 0;
    }
    path = client->offer_path;
  }
  if (http_request(client, "GET", path, NULL, &response) != 0 || response.status != 200 || response.body_len == 0) {
    return 0;
  }
  if (client->is_offerer) {
    peer_connection_set_remote_description(client->pc, response.body, SDP_TYPE_ANSWER);
    client->answer_received = 1;
    printf("[SIGNALING] received answer\n");
  } else {
    peer_connection_set_remote_description(client->pc, response.body, SDP_TYPE_OFFER);
    client->offer_received = 1;
    peer_connection_create_answer(client->pc);
    printf("[SIGNALING] received offer\n");
  }
  return 0;
}

void signaling_client_disconnect(SignalingClient* client) {
  if (client) {
    memset(client, 0, sizeof(*client));
  }
}

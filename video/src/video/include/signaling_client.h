#ifndef SIGNALING_CLIENT_H_
#define SIGNALING_CLIENT_H_

#include <stddef.h>

#include "peer_connection.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SignalingClient {
  PeerConnection* pc;
  void* audio_user_data;
  void* video_user_data;
  int is_offerer;
  int offer_sent;
  int answer_sent;
  int offer_received;
  int answer_received;
  unsigned long long next_poll_ms;
  int port;
  char host[256];
  char base_path[512];
  char offer_path[512];
  char answer_path[512];
} SignalingClient;

int signaling_client_connect(SignalingClient* client,
                             const char* url,
                             PeerConnection* pc,
                             int is_offerer);
int signaling_client_loop(SignalingClient* client);
void signaling_client_disconnect(SignalingClient* client);

#ifdef __cplusplus
}
#endif

#endif  // SIGNALING_CLIENT_H_
